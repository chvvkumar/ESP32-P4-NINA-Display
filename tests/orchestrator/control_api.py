"""Control API — HTTP server for monitoring and controlling the running test."""
import asyncio
import json
import logging
import time

import aiohttp
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
        self._completed = False
        self._final_report: str | None = None

    async def start(self, shutdown_event: asyncio.Event):
        """Start the control API server."""
        self._shutdown_event = shutdown_event
        app = web.Application()
        app.router.add_get("/api/status", self._handle_status)
        app.router.add_get("/api/violations", self._handle_violations)
        app.router.add_get("/api/report", self._handle_report)
        app.router.add_get("/api/devices", self._handle_devices)
        app.router.add_get("/api/simulator", self._handle_simulator)
        app.router.add_get("/api/devices/{host}/perf", self._handle_device_perf)
        app.router.add_get("/api/history", self._handle_history)
        app.router.add_get("/api/snapshots", self._handle_snapshots)
        app.router.add_get("/api/snapshots/{filename}", self._handle_snapshot_file)
        app.router.add_post("/api/intensity", self._handle_intensity)
        app.router.add_post("/api/stop", self._handle_stop)

        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self.bind_address, self.port)
        await site.start()
        logger.info(f"Control API listening on {self.bind_address}:{self.port}")

    def mark_completed(self, final_report: str):
        """Mark the test as completed so status reflects final state."""
        self._completed = True
        self._final_report = final_report
        logger.info("Test marked as completed — API remains available")

    def set_shutdown_event(self, event: asyncio.Event):
        """Replace the shutdown event for post-completion shutdown."""
        self._shutdown_event = event

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

        status = {
            "phase": "completed" if self._completed else pm.get_current_phase().value,
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
        }
        if self._completed:
            status["completed"] = True

        return web.json_response(status)

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
        if self._completed and self._final_report:
            return web.Response(text=self._final_report, content_type="text/plain")
        report = self.report_generator.generate()
        return web.Response(text=report, content_type="text/plain")

    async def _handle_devices(self, request: web.Request) -> web.Response:
        """GET /api/devices — latest metrics per device."""
        devices = {}
        for device in self.config.get("devices", []):
            host = device["host"]
            metrics = self.metrics_collector.get_latest_metrics(host)
            # Return all metrics except raw perf blob
            filtered = {k: v for k, v in metrics.items() if k != "_raw_perf"}
            # Ensure crash info is surfaced at top level for convenience
            if "crash_count" not in filtered and metrics.get("crash_count") is not None:
                filtered["crash_count"] = metrics.get("crash_count", 0)
            if "last_reset_reason" not in filtered and metrics.get("last_reset_reason") is not None:
                filtered["last_reset_reason"] = metrics.get("last_reset_reason", "")
            devices[host] = filtered
        return web.json_response(devices)

    async def _handle_device_perf(self, request: web.Request) -> web.Response:
        """GET /api/devices/{host}/perf — full perf snapshot proxy."""
        host = request.match_info["host"]
        # Try cached perf data first
        metrics = self.metrics_collector.get_latest_metrics(host)
        if metrics.get("_raw_perf"):
            return web.json_response(metrics["_raw_perf"])
        # Fallback: fetch live from device
        try:
            async with aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=8)
            ) as session:
                async with session.get(f"http://{host}/api/perf") as resp:
                    if resp.status == 200:
                        data = await resp.json()
                        return web.json_response(data)
                    return web.json_response({"error": f"Device returned {resp.status}"}, status=502)
        except Exception as e:
            return web.json_response({"error": str(e)}, status=502)

    async def _handle_history(self, request: web.Request) -> web.Response:
        """GET /api/history?host=X&limit=N — metrics time series."""
        host = request.query.get("host")
        limit = int(request.query.get("limit", 100))

        if host:
            history = self.metrics_collector.get_metrics_history(host)
            # Strip _raw_perf from history entries
            cleaned = [{k: v for k, v in entry.items() if k != "_raw_perf"} for entry in history[-limit:]]
            return web.json_response({host: cleaned})

        # All devices
        result = {}
        for device in self.config.get("devices", []):
            h = device["host"]
            history = self.metrics_collector.get_metrics_history(h)
            result[h] = [{k: v for k, v in entry.items() if k != "_raw_perf"} for entry in history[-limit:]]
        return web.json_response(result)

    async def _handle_snapshots(self, request: web.Request) -> web.Response:
        """GET /api/snapshots — list violation snapshots."""
        import os
        snapshot_dir = os.path.join("logs", "violations")
        if not os.path.exists(snapshot_dir):
            return web.json_response([])
        files = sorted(os.listdir(snapshot_dir))
        return web.json_response(files)

    async def _handle_snapshot_file(self, request: web.Request) -> web.Response:
        """GET /api/snapshots/{filename} — download a snapshot file."""
        import os
        filename = request.match_info["filename"]
        # Prevent path traversal
        if ".." in filename or "/" in filename or "\\" in filename:
            return web.json_response({"error": "Invalid filename"}, status=400)
        filepath = os.path.join("logs", "violations", filename)
        if not os.path.exists(filepath):
            return web.json_response({"error": "Not found"}, status=404)
        if filename.endswith(".json"):
            with open(filepath) as f:
                return web.json_response(json.load(f))
        else:
            with open(filepath, "rb") as f:
                return web.Response(body=f.read(), content_type="application/octet-stream")

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
