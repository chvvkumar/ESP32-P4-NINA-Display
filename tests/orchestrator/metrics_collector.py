"""Metrics collector -- polls device health every 10s, writes to InfluxDB."""
import asyncio
import logging
import time
from collections import deque

import aiohttp

logger = logging.getLogger(__name__)


class MetricsCollector:
    """Polls device endpoints and writes measurements to InfluxDB."""

    def __init__(self, devices: list, influx_writer, phase_manager):
        self.devices = devices
        self.influx_writer = influx_writer
        self.phase_manager = phase_manager

        self._running = False
        self._task: asyncio.Task | None = None
        self._latest_metrics: dict[str, dict] = {}
        self._previous_boot_count: dict[str, int] = {}
        self._poll_interval_s = 10
        # Ring buffer: 360 entries = 1 hour at 10s intervals per device
        self._metrics_history: dict[str, deque] = {}

    async def start(self):
        """Start the metrics collection loop."""
        self._running = True
        self._task = asyncio.create_task(self._run())
        logger.info("Metrics collector started")

    async def stop(self):
        """Stop the metrics collection loop."""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info("Metrics collector stopped")

    def get_latest_metrics(self, device: str) -> dict:
        """Return the latest collected metrics for a device host."""
        return self._latest_metrics.get(device, {})

    def get_metrics_history(self, device: str) -> list[dict]:
        """Return the metrics history for a device host as a list of dicts."""
        if device in self._metrics_history:
            return list(self._metrics_history[device])
        return []

    def get_all_latest_metrics(self) -> dict[str, dict]:
        """Return latest metrics for all devices, keyed by host."""
        return dict(self._latest_metrics)

    async def _run(self):
        """Main polling loop — polls devices sequentially to avoid socket exhaustion."""
        while self._running:
            for device in self.devices:
                try:
                    await self._poll_device(device)
                except Exception as e:
                    logger.debug(f"Poll failed for {device['host']}: {e}")
                await asyncio.sleep(1)  # Brief gap between devices
            await asyncio.sleep(self._poll_interval_s)

    async def _poll_device(self, device: dict):
        """Poll a single device and write metrics to InfluxDB."""
        host = device["host"]
        name = device.get("name", host)
        tags = {"device": name, "host": host}
        phase = self.phase_manager.get_current_phase().value

        request_start = time.time()
        perf_data = None
        status_data = None
        nina_status_data = None

        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=8)
        ) as session:
            # GET /api/perf
            try:
                async with session.get(f"http://{host}/api/perf") as resp:
                    if resp.status == 200:
                        perf_data = await resp.json()
            except Exception as exc:
                logger.debug("Perf poll failed for %s: %s", host, exc)

            # GET /api/status
            try:
                async with session.get(f"http://{host}/api/status") as resp:
                    if resp.status == 200:
                        status_data = await resp.json()
            except Exception as exc:
                logger.debug("Status poll failed for %s: %s", host, exc)

            # GET /api/nina/status
            try:
                async with session.get(f"http://{host}/api/nina/status") as resp:
                    if resp.status == 200:
                        nina_status_data = await resp.json()
            except Exception as exc:
                logger.debug("NINA status poll failed for %s: %s", host, exc)

            # GET /api/crash (optional — may not exist on older firmware)
            crash_data = None
            try:
                async with session.get(
                    f"http://{host}/api/crash",
                    timeout=aiohttp.ClientTimeout(total=3),
                ) as resp:
                    if resp.status == 200:
                        crash_data = await resp.json()
            except Exception:
                pass  # Silently skip — endpoint may not exist

        request_latency_ms = (time.time() - request_start) * 1000

        metrics: dict = {
            "timestamp": time.time(),
            "reachable": perf_data is not None or status_data is not None,
            "phase": phase,
            "canary_latency_ms": request_latency_ms,
        }

        # -- device_health measurement --
        # Primary source: /api/status (top-level heap_free, psram_free, uptime_ms, boot_count)
        # Secondary source: /api/perf (memory.heap_free_bytes etc, nested under "memory")
        heap_free = 0
        psram_free = 0
        heap_min = 0
        psram_min = 0
        uptime_s = 0.0
        boot_count = 0

        if status_data:
            uptime_s = status_data.get("uptime_ms", 0) / 1000.0
            boot_count = status_data.get("boot_count", 0)

        if perf_data and perf_data.get("enabled"):
            mem = perf_data.get("memory", {})
            if mem:
                # Use /api/perf for accurate memory — /api/status heap_free
                # includes PSRAM on ESP32-P4 (esp_get_free_heap_size bug)
                heap_free = mem.get("heap_free_bytes", 0)
                heap_min = mem.get("heap_min_free_bytes", 0)
                psram_free = mem.get("psram_free_bytes", 0)
                psram_min = mem.get("psram_min_free_bytes", 0)

        metrics.update({
            "heap_free": heap_free,
            "psram_free": psram_free,
            "heap_min": heap_min,
            "psram_min": psram_min,
            "uptime_s": uptime_s,
            "boot_count": boot_count,
        })

        # Detect reboot
        prev_boot = self._previous_boot_count.get(host)
        if prev_boot is not None and boot_count > prev_boot:
            metrics["reboot_detected"] = True
            logger.warning("Reboot detected on %s (boot_count %d -> %d)",
                           host, prev_boot, boot_count)
        elif prev_boot is not None and uptime_s < 30:
            metrics["reboot_detected"] = True
        else:
            metrics["reboot_detected"] = False
        self._previous_boot_count[host] = boot_count

        if heap_free > 0 or psram_free > 0:
            self.influx_writer.write(
                "device_health",
                tags,
                {
                    "heap_free": heap_free,
                    "heap_min": heap_min,
                    "psram_free": psram_free,
                    "psram_min": psram_min,
                    "uptime_s": uptime_s,
                    "boot_count": boot_count,
                    "phase": phase,
                },
            )

        # -- device_wifi measurement (from perf.wifi) --
        if perf_data and perf_data.get("enabled"):
            wifi = perf_data.get("wifi", {})
            if wifi:
                rssi = float(wifi.get("rssi", 0))
                rssi_avg = float(wifi.get("rssi_avg", 0))
                disc = wifi.get("disconnect_count", {})
                disc_count = disc.get("total", 0) if isinstance(disc, dict) else 0

                metrics.update({
                    "rssi": rssi,
                    "rssi_avg": rssi_avg,
                    "wifi_disconnect_count": disc_count,
                })

                self.influx_writer.write(
                    "device_wifi",
                    tags,
                    {
                        "rssi": rssi,
                        "rssi_avg": rssi_avg,
                        "disconnect_count": disc_count,
                        "phase": phase,
                    },
                )

        # -- device_poll canary measurement --
        self.influx_writer.write(
            "device_poll",
            tags,
            {
                "canary_latency_ms": request_latency_ms,
                "reachable": 1 if metrics["reachable"] else 0,
                "phase": phase,
            },
        )

        # -- HTTP performance metrics (from perf.network) --
        if perf_data and perf_data.get("enabled"):
            network = perf_data.get("network", {})
            if network:
                req_counter = network.get("http_request_count", {})
                fail_counter = network.get("http_failure_count", {})
                retry_counter = network.get("http_retry_count", {})
                req_timer = network.get("http_request", {})

                request_count = req_counter.get("total", 0)
                failure_count = fail_counter.get("total", 0)
                retry_count = retry_counter.get("total", 0)
                avg_ms = req_timer.get("avg_ms", 0.0)
                max_ms = req_timer.get("max_ms", 0.0)

                http_metrics = {
                    "request_count": request_count,
                    "failure_count": failure_count,
                    "retry_count": retry_count,
                    "avg_ms": avg_ms,
                    "max_ms": max_ms,
                }
                metrics["http"] = http_metrics

                self.influx_writer.write(
                    "device_http",
                    tags,
                    {**http_metrics, "phase": phase},
                )

        # -- nina_instance measurements --
        if nina_status_data:
            instances = nina_status_data.get("instances", [])
            metrics["nina_status"] = nina_status_data
            for inst in instances:
                idx = inst.get("index", 0)
                is_connected = inst.get("connection_state") == "connected"
                ws_connected = inst.get("websocket_connected", False)
                poll_ms = inst.get("last_successful_poll_ms", 0)
                age_ms = int(time.time() * 1000) - poll_ms if poll_ms > 0 else 0
                self.influx_writer.write(
                    "nina_instance",
                    {**tags, "instance": str(idx), "phase": phase},
                    {
                        "connected": 1 if is_connected else 0,
                        "ws_connected": 1 if ws_connected else 0,
                        "consecutive_failures": inst.get("consecutive_failures", 0),
                        "last_poll_age_ms": age_ms,
                    },
                )

        # -- Extended perf_data extraction --
        if perf_data and perf_data.get("enabled"):
            # Store raw perf_data for control API passthrough
            metrics["_raw_perf"] = perf_data

            # CPU metrics
            cpu = perf_data.get("cpu", {})
            if cpu:
                core_load = cpu.get("core_load", [0.0, 0.0])
                metrics["core0_load"] = core_load[0] if len(core_load) > 0 else 0.0
                metrics["core1_load"] = core_load[1] if len(core_load) > 1 else 0.0
                metrics["total_load"] = cpu.get("total_load", 0.0)
                metrics["task_count"] = cpu.get("task_count", 0)
                metrics["lvgl_render_avg_ms"] = cpu.get("lvgl_render_avg_ms", 0.0)

                # Task stack HWMs — only tasks with cpu_percent > 0
                tasks = cpu.get("tasks", [])
                if tasks:
                    metrics["tasks"] = [
                        {
                            "name": t.get("name", ""),
                            "stack_hwm": t.get("stack_hwm", 0),
                            "cpu_percent": t.get("cpu_percent", 0.0),
                        }
                        for t in tasks
                        if t.get("cpu_percent", 0) > 0
                    ]

            # Memory largest free blocks
            mem = perf_data.get("memory", {})
            if mem:
                metrics["heap_largest_free_block"] = mem.get("heap_largest_free_block", 0)
                metrics["psram_largest_free_block"] = mem.get("psram_largest_free_block", 0)

            # UI metrics
            ui = perf_data.get("ui", {})
            if ui:
                lock_wait = ui.get("ui_lock_wait", {})
                metrics["ui_lock_wait_avg_ms"] = lock_wait.get("avg_ms", 0.0)
                metrics["ui_lock_wait_max_ms"] = lock_wait.get("max_ms", 0.0)

            # Per-endpoint avg_ms (only endpoints with count > 0)
            endpoints_raw = perf_data.get("endpoints", {})
            if endpoints_raw:
                endpoints = {}
                for ep_name, ep_data in endpoints_raw.items():
                    if isinstance(ep_data, dict) and ep_data.get("count", 0) > 0:
                        endpoints[ep_name] = ep_data.get("avg_ms", 0.0)
                if endpoints:
                    metrics["endpoints"] = endpoints

            # Poll cycle metrics
            poll_cycle = perf_data.get("poll_cycle", {})
            if poll_cycle:
                metrics["poll_cycle_avg_ms"] = poll_cycle.get("avg_ms", 0.0)
                metrics["poll_cycle_max_ms"] = poll_cycle.get("max_ms", 0.0)

        # -- Crash info (from /api/crash, optional endpoint) --
        if crash_data:
            metrics["crash_count"] = crash_data.get("crash_count", 0)
            metrics["last_reset_reason"] = crash_data.get("last_reset_reason", "unknown")
            metrics["last_reset_reason_code"] = crash_data.get("last_reset_reason_code", 0)
            metrics["boot_count_crash"] = crash_data.get("boot_count", 0)

        # -- Append condensed snapshot to history ring buffer --
        if host not in self._metrics_history:
            self._metrics_history[host] = deque(maxlen=360)

        # Build condensed snapshot (exclude bulky _raw_perf and nina_status)
        snapshot = {k: v for k, v in metrics.items()
                    if k not in ("_raw_perf", "nina_status")}
        self._metrics_history[host].append(snapshot)

        self._latest_metrics[host] = metrics
