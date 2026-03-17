"""NINA instance monitor — verifies all simulator instances stay connected."""
import asyncio
import time
import logging
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)


class NinaMonitorWorkload(BaseWorkload):
    """Polls /api/nina/status and verifies all instances stay connected."""

    def __init__(self, devices: list[dict], metrics_writer):
        super().__init__("nina_monitor", devices, metrics_writer)
        self._disconnected_since: dict[str, dict[int, float | None]] = {}
        self._last_poll_ms: dict[str, dict[int, int]] = {}

    async def _run_loop(self, device: dict, intensity: float):
        """Override: fixed 10s polling interval."""
        host = device["host"]
        self._disconnected_since[host] = {}
        self._last_poll_ms[host] = {}

        while not self._stop_event.is_set():
            start = time.monotonic()
            try:
                await self._check_instances(host)
            except Exception as e:
                logger.warning(f"{self.name} error on {host}: {e}")
                self._record_error(host, str(e))

            try:
                await asyncio.wait_for(self._stop_event.wait(), timeout=10.0)
                break
            except asyncio.TimeoutError:
                pass

    async def _check_instances(self, host: str):
        """Check all NINA instances on a device."""
        start = time.monotonic()

        async with self._session.get(
            f"http://{host}/api/nina/status"
        ) as resp:
            elapsed_ms = (time.monotonic() - start) * 1000
            self._record_result(host, elapsed_ms, resp.status)

            if resp.status != 200:
                return

            data = await resp.json()

        now = time.monotonic()

        for inst in data.get("instances", []):
            idx = inst["index"]
            connected = inst.get("connection_state") == "connected"
            ws_connected = inst.get("websocket_connected", False)
            failures = inst.get("consecutive_failures", 0)
            poll_ms = inst.get("last_successful_poll_ms", 0)

            # Track disconnection duration
            if not connected:
                if self._disconnected_since[host].get(idx) is None:
                    self._disconnected_since[host][idx] = now
                    logger.warning(
                        f"{host} instance {idx}: disconnected"
                    )
                disconnected_for = now - self._disconnected_since[host][idx]
                if disconnected_for > 60:
                    logger.error(
                        f"{host} instance {idx}: disconnected for "
                        f"{disconnected_for:.0f}s (>60s)"
                    )
            else:
                if self._disconnected_since[host].get(idx) is not None:
                    logger.info(f"{host} instance {idx}: reconnected")
                self._disconnected_since[host][idx] = None

            # Check poll_ms is advancing
            prev_poll = self._last_poll_ms[host].get(idx, 0)
            if poll_ms > 0 and poll_ms == prev_poll and connected:
                logger.warning(
                    f"{host} instance {idx}: last_successful_poll_ms stale "
                    f"({poll_ms})"
                )
            self._last_poll_ms[host][idx] = poll_ms

            # Write per-instance metrics
            last_poll_age_ms = int(time.time() * 1000) - poll_ms if poll_ms > 0 else 0
            self.metrics_writer.write(
                "nina_instance",
                tags={
                    "device": host,
                    "instance": str(idx),
                },
                fields={
                    "connected": connected,
                    "ws_connected": ws_connected,
                    "consecutive_failures": failures,
                    "last_poll_age_ms": last_poll_age_ms,
                },
            )

    def get_disconnected_duration(self, host: str, idx: int) -> float | None:
        """Return how long an instance has been disconnected, or None if connected."""
        since = self._disconnected_since.get(host, {}).get(idx)
        if since is None:
            return None
        return time.monotonic() - since
