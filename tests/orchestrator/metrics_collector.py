"""Metrics collector -- polls device health every 10s, writes to InfluxDB."""
import asyncio
import logging
import time

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

    async def _run(self):
        """Main polling loop."""
        while self._running:
            tasks = [self._poll_device(d) for d in self.devices]
            await asyncio.gather(*tasks, return_exceptions=True)
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
            heap_free = status_data.get("heap_free", 0)
            psram_free = status_data.get("psram_free", 0)
            uptime_s = status_data.get("uptime_ms", 0) / 1000.0
            boot_count = status_data.get("boot_count", 0)

        if perf_data and perf_data.get("enabled"):
            mem = perf_data.get("memory", {})
            if mem:
                # Don't overwrite heap_free/psram_free — /api/status values are
                # identical and already set above. Only grab min watermarks from perf.
                heap_min = mem.get("heap_min_free_bytes", 0)
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
                disc_count = disc.get("count", 0) if isinstance(disc, dict) else 0

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
                    "p95_ms": max_ms,  # Use max as p95 approximation
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

        self._latest_metrics[host] = metrics
