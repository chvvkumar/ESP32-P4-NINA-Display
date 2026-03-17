"""Concurrent connections workload — tests connection limits."""
import asyncio
import time
import logging
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)


class ConcurrentConnsWorkload(BaseWorkload):
    """Opens N simultaneous connections to test device limits."""

    def __init__(self, devices: list[dict], metrics_writer,
                 endpoint: str = "/api/perf"):
        super().__init__("concurrent_conns", devices, metrics_writer)
        self.endpoint = endpoint

    async def _run_loop(self, device: dict, intensity: float):
        """Override: connection count pattern instead of RPS."""
        host = device["host"]
        target_count = int(intensity)

        while not self._stop_event.is_set():
            # Ramp from 1 to target_count over 10 seconds
            for n in range(1, target_count + 1):
                if self._stop_event.is_set():
                    return

                successes = 0
                failures = 0
                start = time.monotonic()

                # Open N concurrent requests
                tasks = []
                for _ in range(n):
                    tasks.append(self._single_request(host))

                results = await asyncio.gather(*tasks, return_exceptions=True)

                for result in results:
                    if isinstance(result, Exception):
                        failures += 1
                    elif isinstance(result, tuple):
                        status, elapsed_ms = result
                        if 200 <= status < 500:
                            successes += 1
                        else:
                            failures += 1

                total = successes + failures
                success_rate = successes / max(total, 1) * 100

                self.metrics_writer.write(
                    "workload_result",
                    tags={"device": host, "workload": self.name},
                    fields={
                        "response_ms": (time.monotonic() - start) * 1000,
                        "status_code": 200 if successes > 0 else 0,
                        "error": "",
                        "concurrent_count": n,
                        "success_count": successes,
                        "failure_count": failures,
                        "success_rate": success_rate,
                    },
                )

                # Brief pause between ramp steps
                ramp_delay = 10.0 / max(target_count, 1)
                try:
                    await asyncio.wait_for(
                        self._stop_event.wait(), timeout=ramp_delay
                    )
                    return
                except asyncio.TimeoutError:
                    pass

            # Wait between rounds
            try:
                await asyncio.wait_for(self._stop_event.wait(), timeout=5.0)
                return
            except asyncio.TimeoutError:
                pass

    async def _single_request(self, host: str) -> tuple[int, float]:
        """Make a single request with its own session."""
        timeout = aiohttp.ClientTimeout(total=5)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            start = time.monotonic()
            async with session.get(f"http://{host}{self.endpoint}") as resp:
                await resp.read()
                return resp.status, (time.monotonic() - start) * 1000
