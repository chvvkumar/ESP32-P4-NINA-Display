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
        if perf_data:
            heap_free = perf_data.get("heap_free", 0)
            psram_free = perf_data.get("psram_free", 0)
            uptime_s = perf_data.get("uptime_s", 0)
            core0_load = perf_data.get("core0_load", 0)
            core1_load = perf_data.get("core1_load", 0)
            task_count = perf_data.get("task_count", 0)
            boot_count = perf_data.get("boot_count", 0)

            metrics.update({
                "heap_free": heap_free,
                "psram_free": psram_free,
                "uptime_s": uptime_s,
                "core0_load": core0_load,
                "core1_load": core1_load,
                "task_count": task_count,
                "boot_count": boot_count,
            })

            # Detect reboot
            prev_boot = self._previous_boot_count.get(host)
            if prev_boot is not None and boot_count > prev_boot:
                metrics["reboot_detected"] = True
                logger.warning("Reboot detected on %s (boot_count %d -> %d)",
                               host, prev_boot, boot_count)
            elif prev_boot is not None and uptime_s < 30:
                # Uptime reset also suggests reboot
                metrics["reboot_detected"] = True
            else:
                metrics["reboot_detected"] = False
            self._previous_boot_count[host] = boot_count

            self.influx_writer.write(
                "device_health",
                tags,
                {
                    "heap_free": heap_free,
                    "psram_free": psram_free,
                    "uptime_s": uptime_s,
                    "core0_load": core0_load,
                    "core1_load": core1_load,
                    "task_count": task_count,
                    "boot_count": boot_count,
                    "phase": phase,
                },
            )

        # -- device_wifi measurement --
        if status_data:
            rssi = status_data.get("rssi", 0)
            wifi_connected = status_data.get("wifi_connected", False)
            ip = status_data.get("ip", "")

            metrics.update({
                "rssi": rssi,
                "wifi_connected": wifi_connected,
                "ip": ip,
            })

            self.influx_writer.write(
                "device_wifi",
                tags,
                {
                    "rssi": rssi,
                    "wifi_connected": 1 if wifi_connected else 0,
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

        # -- HTTP performance metrics --
        if perf_data and "http" in perf_data:
            http = perf_data["http"]
            self.influx_writer.write(
                "device_http",
                tags,
                {
                    "requests_total": http.get("requests_total", 0),
                    "errors_total": http.get("errors_total", 0),
                    "avg_latency_ms": http.get("avg_latency_ms", 0),
                    "p95_latency_ms": http.get("p95_latency_ms", 0),
                    "phase": phase,
                },
            )
            metrics["http"] = http

        # -- nina_instance measurements --
        if nina_status_data:
            instances = nina_status_data.get("instances", [])
            metrics["instances"] = instances
            for i, inst in enumerate(instances):
                self.influx_writer.write(
                    "nina_instance",
                    {**tags, "instance": str(i)},
                    {
                        "connected": 1 if inst.get("connected", False) else 0,
                        "state": inst.get("state", "unknown"),
                        "phase": phase,
                    },
                )

        self._latest_metrics[host] = metrics
