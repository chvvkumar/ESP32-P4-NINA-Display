"""Page cycle workload — navigates through all device pages."""
import asyncio
import time
import logging
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)

EXTRA_PAGES = 5  # AllSky, Spotify, Summary, Settings, SysInfo


class PageCycleWorkload(BaseWorkload):
    """Cycles through pages with configurable dwell time."""

    def __init__(self, devices: list[dict], metrics_writer, dwell_s: float = 2.0):
        super().__init__("page_cycle", devices, metrics_writer)
        self.dwell_s = dwell_s
        self._max_pages: dict[str, int] = {}

    async def _run_loop(self, device: dict, intensity: float):
        """Override: dwell-based timing instead of RPS."""
        host = device["host"]
        interval = intensity  # intensity is interval_s for this workload
        current_page = 0

        # Discover max pages
        try:
            async with self._session.get(f"http://{host}/api/status") as resp:
                if resp.status == 200:
                    data = await resp.json()
                    instance_count = data.get("instance_count", 1)
                    self._max_pages[host] = instance_count + EXTRA_PAGES
        except Exception:
            self._max_pages[host] = 8  # Reasonable default

        max_pages = self._max_pages.get(host, 8)

        while not self._stop_event.is_set():
            start = time.monotonic()
            try:
                # Navigate to page
                async with self._session.post(
                    f"http://{host}/api/page",
                    json={"page": current_page},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)

                # Wait for LVGL animation
                await asyncio.sleep(0.5)

                # Verify page changed
                async with self._session.get(
                    f"http://{host}/api/status"
                ) as resp:
                    if resp.status == 200:
                        data = await resp.json()
                        actual_page = data.get("active_page", -1)
                        if actual_page != current_page:
                            logger.debug(
                                f"{host}: page mismatch, expected {current_page} "
                                f"got {actual_page}"
                            )

            except Exception as e:
                logger.warning(f"{self.name} error on {host}: {e}")
                self._record_error(host, str(e))

            current_page = (current_page + 1) % max_pages

            # Dwell on page
            try:
                await asyncio.wait_for(
                    self._stop_event.wait(), timeout=max(0, interval - 0.5)
                )
                break
            except asyncio.TimeoutError:
                pass
