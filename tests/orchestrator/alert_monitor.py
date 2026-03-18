"""Alert monitor -- evaluates thresholds, logs violations."""
import asyncio
import enum
import json
import logging
import os
import time
from collections import deque
from dataclasses import dataclass

import aiohttp

logger = logging.getLogger(__name__)


class Severity(enum.Enum):
    CRITICAL = "CRITICAL"
    HIGH = "HIGH"
    MEDIUM = "MEDIUM"
    LOW = "LOW"


@dataclass
class Violation:
    timestamp: float
    device: str
    check_name: str
    severity: Severity
    message: str
    value: float
    threshold: float


class AlertMonitor:
    """Evaluates health thresholds and records violations."""

    def __init__(self, thresholds: dict, metrics_collector, influx_writer, phase_manager):
        self.thresholds = thresholds
        self.metrics_collector = metrics_collector
        self.influx_writer = influx_writer
        self.phase_manager = phase_manager

        self._violations: list[Violation] = []
        self._critical_count = 0
        self._running = False
        self._task: asyncio.Task | None = None

        # Memory leak tracking: deque of (timestamp, heap_free) per device
        self._heap_history: dict[str, deque] = {}
        self._psram_history: dict[str, deque] = {}

        # Unreachable tracking: last reachable timestamp per device
        self._last_reachable: dict[str, float] = {}
        self._unreachable_retries: dict[str, int] = {}

        # Instance down tracking: {device: {instance_idx: first_down_ts}}
        self._instance_down_since: dict[str, dict[int, float]] = {}

        os.makedirs("logs/violations", exist_ok=True)

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    async def start(self):
        self._running = True
        self._task = asyncio.create_task(self._run())
        logger.info("Alert monitor started")

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info("Alert monitor stopped")

    def get_violations(self) -> list[Violation]:
        return list(self._violations)

    def get_critical_count(self) -> int:
        return self._critical_count

    def has_critical_violations(self) -> bool:
        return self._critical_count > 0

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    async def _run(self):
        while self._running:
            devices = self.metrics_collector.devices
            for device in devices:
                host = device["host"]
                metrics = self.metrics_collector.get_latest_metrics(host)
                if metrics:
                    await self.evaluate(host, metrics)
            await asyncio.sleep(10)

    # ------------------------------------------------------------------
    # Evaluation
    # ------------------------------------------------------------------

    async def evaluate(self, device: str, metrics: dict):
        """Run all threshold checks against the latest metrics."""
        now = time.time()
        phase = self.phase_manager.get_current_phase().value

        # 1. Reboot detected (only if device is reachable — unreachable gives false 0 values)
        if metrics.get("reachable", False) and metrics.get("reboot_detected", False):
            reboot_msg = "Device rebooted unexpectedly"
            # Try to fetch crash reason from the device
            crash_reason = metrics.get("last_reset_reason")
            if not crash_reason:
                crash_reason = await self._fetch_crash_reason(device)
            if crash_reason:
                reboot_msg = f"Device rebooted unexpectedly (reason: {crash_reason})"
            await self._record_violation(
                device, "reboot_detected", Severity.CRITICAL,
                reboot_msg,
                metrics.get("boot_count", 0), 0,
            )
            await self._capture_state_snapshot(device)

        # 2. Heap exhaustion (skip if device unreachable — 0 is a failed read, not real)
        heap_free = metrics.get("heap_free", float("inf"))
        heap_threshold = self.thresholds.get("heap_min_free_bytes", 8192)
        if 0 < heap_free < heap_threshold:
            await self._record_violation(
                device, "heap_critical", Severity.CRITICAL,
                f"Heap critically low: {heap_free} bytes",
                heap_free, heap_threshold,
            )
            await self._capture_state_snapshot(device)

        # 3. PSRAM exhaustion (skip if 0 — means failed read)
        psram_free = metrics.get("psram_free", float("inf"))
        psram_threshold = self.thresholds.get("psram_min_free_bytes", 20971520)
        if 0 < psram_free < psram_threshold:
            await self._record_violation(
                device, "psram_critical", Severity.CRITICAL,
                f"PSRAM critically low: {psram_free} bytes",
                psram_free, psram_threshold,
            )
            await self._capture_state_snapshot(device)

        # 4. Unreachable >30s (retry 3x)
        if metrics.get("reachable", True):
            self._last_reachable[device] = now
            self._unreachable_retries[device] = 0
        else:
            last = self._last_reachable.get(device, now)
            if now - last > 30:
                retries = self._unreachable_retries.get(device, 0)
                if retries >= 3:
                    await self._record_violation(
                        device, "unreachable", Severity.CRITICAL,
                        f"Device unreachable for {now - last:.0f}s after 3 retries",
                        now - last, 30,
                    )
                    await self._capture_state_snapshot(device)
                    self._unreachable_retries[device] = 0
                else:
                    self._unreachable_retries[device] = retries + 1

        # 5. HTTP error rate
        http = metrics.get("http", {})
        total_req = http.get("request_count", 0)
        total_err = http.get("failure_count", 0)
        if total_req > 0:
            error_rate = (total_err / total_req) * 100
            if phase == "stress" and error_rate > 5:
                await self._record_violation(
                    device, "http_error_rate_stress", Severity.HIGH,
                    f"HTTP error rate {error_rate:.1f}% during stress (>5%)",
                    error_rate, 5,
                )
            elif phase == "soak" and error_rate > 1:
                await self._record_violation(
                    device, "http_error_rate_soak", Severity.HIGH,
                    f"HTTP error rate {error_rate:.1f}% during soak (>1%)",
                    error_rate, 1,
                )

        # 6. API latency (avg_ms — cumulative average, best steady-state proxy)
        avg_latency = http.get("avg_ms", 0)
        stress_latency_threshold = self.thresholds.get("api_latency_avg_stress_ms", 500)
        soak_latency_threshold = self.thresholds.get("api_latency_avg_soak_ms", 200)
        if phase == "stress" and avg_latency > stress_latency_threshold:
            await self._record_violation(
                device, "api_latency_stress", Severity.HIGH,
                f"API avg latency {avg_latency:.0f}ms during stress (>{stress_latency_threshold}ms)",
                avg_latency, stress_latency_threshold,
            )
        elif phase == "soak" and avg_latency > soak_latency_threshold:
            await self._record_violation(
                device, "api_latency_soak", Severity.HIGH,
                f"API avg latency {avg_latency:.0f}ms during soak (>{soak_latency_threshold}ms)",
                avg_latency, soak_latency_threshold,
            )

        # 7. NINA instance down >60s
        nina_status = metrics.get("nina_status", {})
        instances = nina_status.get("instances", []) if isinstance(nina_status, dict) else []
        if device not in self._instance_down_since:
            self._instance_down_since[device] = {}
        for i, inst in enumerate(instances):
            connected = inst.get("connection_state") == "connected"
            if not connected:
                if i not in self._instance_down_since[device]:
                    self._instance_down_since[device][i] = now
                elif now - self._instance_down_since[device][i] > 60:
                    down_s = now - self._instance_down_since[device][i]
                    await self._record_violation(
                        device, f"nina_instance_{i}_down", Severity.HIGH,
                        f"NINA instance {i} down for {down_s:.0f}s",
                        down_s, 60,
                    )
            else:
                self._instance_down_since[device].pop(i, None)

        # 8. WiFi disconnect during soak (detect via disconnect_count increment)
        if phase == "soak" and metrics.get("wifi_disconnect_count", 0) > 0:
            await self._record_violation(
                device, "wifi_disconnect_soak", Severity.MEDIUM,
                "WiFi disconnected during soak phase",
                0, 0,
            )

        # 9. Heap leak >1KB/hr over 6h
        if heap_free != float("inf"):
            if device not in self._heap_history:
                self._heap_history[device] = deque(maxlen=2160)  # 6h @ 10s
            self._heap_history[device].append((now, heap_free))
            leak_rate = self._compute_leak_rate(self._heap_history[device], 21600)
            if leak_rate is not None and leak_rate < -1024:
                await self._record_violation(
                    device, "heap_leak", Severity.MEDIUM,
                    f"Heap leak detected: {abs(leak_rate):.0f} bytes/hr over 6h",
                    abs(leak_rate), 1024,
                )

        # 10. PSRAM leak >10KB/hr over 6h
        if psram_free != float("inf"):
            if device not in self._psram_history:
                self._psram_history[device] = deque(maxlen=2160)
            self._psram_history[device].append((now, psram_free))
            leak_rate = self._compute_leak_rate(self._psram_history[device], 21600)
            if leak_rate is not None and leak_rate < -10240:
                await self._record_violation(
                    device, "psram_leak", Severity.MEDIUM,
                    f"PSRAM leak detected: {abs(leak_rate):.0f} bytes/hr over 6h",
                    abs(leak_rate), 10240,
                )

        # 11. Heap fragmentation — ratio = largest_free_block / total_free; lower = more fragmented
        heap_frag = metrics.get("heap_frag_ratio", 0)
        heap_frag_min = self.thresholds.get("heap_frag_ratio_min", 0.7)
        if 0 < heap_frag < heap_frag_min:
            await self._record_violation(
                device, "heap_frag_warning", Severity.MEDIUM,
                f"Heap fragmentation high: ratio {heap_frag:.3f} (min {heap_frag_min})",
                heap_frag, heap_frag_min,
            )

        # 12. PSRAM fragmentation (skip if 0 — means no data)
        psram_frag = metrics.get("psram_frag_ratio", 0)
        psram_frag_min = self.thresholds.get("psram_frag_ratio_min", 0.8)
        if 0 < psram_frag < psram_frag_min:
            await self._record_violation(
                device, "psram_frag_warning", Severity.LOW,
                f"PSRAM fragmentation high: ratio {psram_frag:.3f} (min {psram_frag_min})",
                psram_frag, psram_frag_min,
            )

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    async def _record_violation(
        self, device: str, check_name: str, severity: Severity,
        message: str, value: float, threshold: float,
    ):
        v = Violation(
            timestamp=time.time(),
            device=device,
            check_name=check_name,
            severity=severity,
            message=message,
            value=value,
            threshold=threshold,
        )
        self._violations.append(v)
        if severity == Severity.CRITICAL:
            self._critical_count += 1
        logger.warning("[%s] %s on %s: %s", severity.value, check_name, device, message)

        self.influx_writer.write(
            "test_violation",
            {"device": device, "check": check_name, "severity": severity.value},
            {"message": message, "value": float(value), "threshold": float(threshold)},
        )

    async def _fetch_crash_reason(self, device: str) -> str | None:
        """Fetch crash reason from the device's /api/crash endpoint."""
        try:
            async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=3)
            ) as session:
                async with session.get(f"http://{device}/api/crash") as resp:
                    if resp.status == 200:
                        data = await resp.json()
                        return data.get("last_reset_reason")
        except Exception:
            pass
        return None

    async def _capture_state_snapshot(self, device: str):
        """Capture perf data + screenshot on critical violation."""
        ts = int(time.time())
        prefix = f"logs/violations/{device}_{ts}"
        try:
            async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=10)
            ) as session:
                # Save perf data
                try:
                    async with session.get(f"http://{device}/api/perf") as resp:
                        if resp.status == 200:
                            perf = await resp.json()
                            with open(f"{prefix}_perf.json", "w") as f:
                                json.dump(perf, f, indent=2)
                except Exception as exc:
                    logger.debug("Failed to capture perf snapshot for %s: %s",
                                 device, exc)

                # Save screenshot
                try:
                    async with session.get(
                        f"http://{device}/api/screenshot"
                    ) as resp:
                        if resp.status == 200:
                            data = await resp.read()
                            with open(f"{prefix}_screenshot.bin", "wb") as f:
                                f.write(data)
                except Exception as exc:
                    logger.debug("Failed to capture screenshot for %s: %s",
                                 device, exc)
        except Exception as exc:
            logger.debug("State snapshot failed for %s: %s", device, exc)

    @staticmethod
    def _compute_leak_rate(
        history: deque, min_span_s: float
    ) -> float | None:
        """Linear regression over history to detect memory leak.

        Returns bytes/hour rate (negative = leak). Returns None if
        insufficient data span.
        """
        if len(history) < 10:
            return None
        t0 = history[0][0]
        t_last = history[-1][0]
        span = t_last - t0
        if span < min_span_s:
            return None

        # Simple linear regression
        n = len(history)
        sum_t = 0.0
        sum_v = 0.0
        sum_tv = 0.0
        sum_t2 = 0.0
        for t, v in history:
            dt = t - t0  # seconds from start
            sum_t += dt
            sum_v += v
            sum_tv += dt * v
            sum_t2 += dt * dt

        denom = n * sum_t2 - sum_t * sum_t
        if denom == 0:
            return None

        slope_per_sec = (n * sum_tv - sum_t * sum_v) / denom
        slope_per_hour = slope_per_sec * 3600
        return slope_per_hour
