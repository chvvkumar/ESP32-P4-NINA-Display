"""Page cycle workload — cycles through NINA instance pages only."""
import asyncio
import time
import logging
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)

# Page layout: 0=AllSky, 1=Spotify, 2=Summary, 3..N+2=NINA instances, N+3=Settings, N+4=SysInfo
# We only want to cycle through the NINA instance pages (indices 3..N+2)
FIRST_NINA_PAGE = 3


class PageCycleWorkload(BaseWorkload):
    """Cycles through NINA instance pages with configurable dwell time."""

    def __init__(self, devices: list[dict], metrics_writer, dwell_s: float = 5.0):
        super().__init__("page_cycle", devices, metrics_writer)
        self.dwell_s = dwell_s
        self._nina_pages: dict[str, list[int]] = {}

    async def _run_loop(self, device: dict, intensity: float):
        """Override: dwell-based timing, NINA pages only."""
        host = device["host"]
        interval = intensity  # intensity is interval_s for this workload

        # Discover NINA instance pages
        try:
            async with self._session.get(f"http://{host}/api/status") as resp:
                if resp.status == 200:
                    data = await resp.json()
                    instance_count = data.get("instance_count", 3)
                    # NINA pages are at indices 3 through 3+instance_count-1
                    self._nina_pages[host] = list(
                        range(FIRST_NINA_PAGE, FIRST_NINA_PAGE + instance_count)
                    )
                    logger.info(
                        f"{host}: NINA pages {self._nina_pages[host]} "
                        f"({instance_count} instances)"
                    )
        except Exception:
            self._nina_pages[host] = [3, 4, 5]  # Default: 3 instances

        pages = self._nina_pages.get(host, [3, 4, 5])
        page_idx = 0

        while not self._stop_event.is_set():
            current_page = pages[page_idx % len(pages)]
            start = time.monotonic()

            try:
                # Navigate to NINA instance page
                async with self._session.post(
                    f"http://{host}/api/page",
                    json={"page": current_page},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)

                # Wait for LVGL animation
                await asyncio.sleep(0.5)

                # Verify page changed and data is flowing
                async with self._session.get(f"http://{host}/api/status") as resp:
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

            page_idx += 1

            # Dwell on page
            try:
                await asyncio.wait_for(
                    self._stop_event.wait(), timeout=max(0, interval - 0.5)
                )
                break
            except asyncio.TimeoutError:
                pass
