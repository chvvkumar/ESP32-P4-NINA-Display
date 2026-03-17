"""Base workload class with throttling and metrics recording."""
import asyncio
import time
import logging
import aiohttp

logger = logging.getLogger(__name__)


class BaseWorkload:
    """Base class for all workloads."""

    def __init__(self, name: str, devices: list[dict], metrics_writer):
        self.name = name
        self.devices = devices
        self.metrics_writer = metrics_writer
        self._stop_event = asyncio.Event()
        self._tasks: list[asyncio.Task] = []
        self._session: aiohttp.ClientSession | None = None

    async def start(self, intensity: float):
        """Start the workload at given intensity."""
        self._stop_event.clear()
        self._session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=10)
        )
        for device in self.devices:
            task = asyncio.create_task(self._run_loop(device, intensity))
            self._tasks.append(task)

    async def stop(self):
        """Stop the workload gracefully."""
        self._stop_event.set()
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
            self._tasks.clear()
        if self._session:
            await self._session.close()
            self._session = None

    async def _run_loop(self, device: dict, intensity: float):
        """Main loop — calls _execute at the configured rate."""
        host = device["host"]
        interval = 1.0 / intensity if intensity > 0 else 60.0
        while not self._stop_event.is_set():
            start = time.monotonic()
            try:
                await self._execute(self._session, host, intensity)
            except Exception as e:
                logger.warning(f"{self.name} error on {host}: {e}")
                self._record_error(host, str(e))
            elapsed = time.monotonic() - start
            sleep_time = max(0, interval - elapsed)
            try:
                await asyncio.wait_for(
                    self._stop_event.wait(), timeout=sleep_time
                )
                break
            except asyncio.TimeoutError:
                pass

    async def _execute(self, session: aiohttp.ClientSession, host: str,
                       intensity: float):
        """Override in subclasses. Perform one unit of work."""
        raise NotImplementedError

    def _record_result(self, host: str, response_ms: float, status_code: int):
        """Record a successful request to InfluxDB."""
        self.metrics_writer.write(
            "workload_result",
            tags={"device": host, "workload": self.name},
            fields={
                "response_ms": response_ms,
                "status_code": status_code,
                "error": "",
            },
        )

    def _record_error(self, host: str, error: str):
        """Record a failed request to InfluxDB."""
        self.metrics_writer.write(
            "workload_result",
            tags={"device": host, "workload": self.name},
            fields={"response_ms": 0.0, "status_code": 0, "error": error},
        )
