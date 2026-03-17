"""HTTP stress workload — hammers GET endpoints."""
import itertools
import time
import logging
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)

ENDPOINTS = ["/api/perf", "/api/version", "/api/config"]


class HttpStressWorkload(BaseWorkload):
    """Cycles through GET endpoints at configured requests per second."""

    def __init__(self, devices: list[dict], metrics_writer):
        super().__init__("http_stress", devices, metrics_writer)
        self._endpoint_cycle = itertools.cycle(ENDPOINTS)

    async def _execute(self, session: aiohttp.ClientSession, host: str,
                       intensity: float):
        endpoint = next(self._endpoint_cycle)
        url = f"http://{host}{endpoint}"
        start = time.monotonic()
        try:
            async with session.get(url) as resp:
                await resp.read()
                elapsed_ms = (time.monotonic() - start) * 1000
                self._record_result(host, elapsed_ms, resp.status)
        except Exception as e:
            elapsed_ms = (time.monotonic() - start) * 1000
            self._record_error(host, str(e))
            raise
