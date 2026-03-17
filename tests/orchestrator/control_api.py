"""Control API — HTTP server for monitoring and controlling the running test."""
import asyncio
import logging
import time
from aiohttp import web

logger = logging.getLogger(__name__)


class ControlAPI:
    """HTTP API for remote test control and monitoring."""

    def __init__(self, phase_manager, workload_manager, metrics_collector,
                 alert_monitor, report_generator, influx_writer, simulator,
                 config: dict, bind_address: str = "0.0.0.0", port: int = 8880):
        self.phase_manager = phase_manager
        self.workload_manager = workload_manager
        self.metrics_collector = metrics_collector
        self.alert_monitor = alert_monitor
        self.report_generator = report_generator
        self.influx_writer = influx_writer
        self.simulator = simulator
        self.config = config
        self.bind_address = bind_address
        self.port = port
        self._runner: web.AppRunner | None = None
        self._shutdown_event: asyncio.Event | None = None

    async def start(self, shutdown_event: asyncio.Event):
        """Start the control API server."""
        self._shutdown_event = shutdown_event
        app = web.Application()
        app.router.add_get("/api/status", self._handle_status)
        app.router.add_get("/api/violations", self._handle_violations)
        app.router.add_get("/api/report", self._handle_report)
        app.router.add_get("/api/devices", self._handle_devices)
        app.router.add_get("/api/simulator", self._handle_simulator)
        app.router.add_post("/api/intensity", self._handle_intensity)
        app.router.add_post("/api/stop", self._handle_stop)

        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self.bind_address, self.port)
        await site.start()
        logger.info(f"Control API listening on {self.bind_address}:{self.port}")

    async def stop(self):
        """Stop the control API server."""
        if self._runner:
            await self._runner.cleanup()
            self._runner = None

    async def _handle_status(self, request: web.Request) -> web.Response:
        """GET /api/status — current test status overview."""
        pm = self.phase_manager
        am = self.alert_monitor
        violations = am.get_violations()

        return web.json_response({
            "phase": pm.get_current_phase().value,
            "phase_elapsed_s": round(pm.get_phase_elapsed_s(), 1),
            "total_elapsed_s": round(pm.get_total_elapsed_s(), 1),
            "cycle": pm.cycle_count,
            "violations": {
                "total": len(violations),
                "critical": am.get_critical_count(),
                "high": sum(1 for v in violations if v.severity.value == "HIGH"),
                "medium": sum(1 for v in violations if v.severity.value == "MEDIUM"),
            },
            "verdict": "FAIL" if am.has_critical_violations() else "PASS",
            "devices": len(self.config.get("devices", [])),
        })

    async def _handle_violations(self, request: web.Request) -> web.Response:
        """GET /api/violations — list of all violations."""
        violations = self.alert_monitor.get_violations()
        limit = int(request.query.get("limit", 100))
        return web.json_response([
            {
                "timestamp": v.timestamp,
                "device": v.device,
                "check": v.check_name,
                "severity": v.severity.value,
                "message": v.message,
                "value": v.value,
                "threshold": v.threshold,
            }
            for v in violations[-limit:]
        ])

    async def _handle_report(self, request: web.Request) -> web.Response:
        """GET /api/report — generate and return current report text."""
        report = self.report_generator.generate()
        return web.Response(text=report, content_type="text/plain")

    async def _handle_devices(self, request: web.Request) -> web.Response:
        """GET /api/devices — latest metrics per device."""
        devices = {}
        for device in self.config.get("devices", []):
            host = device["host"]
            metrics = self.metrics_collector.get_latest_metrics(host)
            devices[host] = {
                "reachable": metrics.get("reachable", False),
                "heap_free": metrics.get("heap_free", 0),
                "psram_free": metrics.get("psram_free", 0),
                "uptime_s": metrics.get("uptime_s", 0),
                "boot_count": metrics.get("boot_count", 0),
                "rssi": metrics.get("rssi", 0),
            }
        return web.json_response(devices)

    async def _handle_simulator(self, request: web.Request) -> web.Response:
        """GET /api/simulator — simulator stats per instance."""
        stats = {}
        for i in range(3):
            stats[f"instance_{i}"] = self.simulator.get_stats(i)
        return web.json_response(stats)

    async def _handle_intensity(self, request: web.Request) -> web.Response:
        """POST /api/intensity — change workload intensity.

        Body: {"mode": "stress"|"soak"|"mini_burst"|"off"}
        """
        try:
            data = await request.json()
            mode = data.get("mode", "")
            if mode == "off":
                await self.workload_manager.stop_all()
                return web.json_response({"status": "ok", "mode": "off"})
            elif mode in ("stress", "soak", "mini_burst"):
                await self.workload_manager.stop_all()
                await self.workload_manager.start_all(mode)
                return web.json_response({"status": "ok", "mode": mode})
            else:
                return web.json_response(
                    {"error": f"Unknown mode: {mode}. Use stress/soak/mini_burst/off"},
                    status=400,
                )
        except Exception as e:
            return web.json_response({"error": str(e)}, status=400)

    async def _handle_stop(self, request: web.Request) -> web.Response:
        """POST /api/stop — graceful shutdown."""
        if self._shutdown_event:
            self._shutdown_event.set()
        return web.json_response({"status": "stopping"})
