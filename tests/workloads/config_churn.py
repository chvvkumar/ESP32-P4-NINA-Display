"""Config churn workload — rapidly changes device settings."""
import random
import time
import logging
import json as json_mod
import aiohttp

from .base import BaseWorkload

logger = logging.getLogger(__name__)


class ConfigChurnWorkload(BaseWorkload):
    """Posts config changes in rotation with periodic negative tests."""

    def __init__(self, devices: list[dict], metrics_writer):
        super().__init__("config_churn", devices, metrics_writer)
        self._request_count: dict[str, int] = {}
        self._theme_cycle: int = 0
        self._widget_cycle: int = 0
        self._saved_configs: dict[str, dict] = {}

    async def save_original_config(self, session: aiohttp.ClientSession,
                                    host: str):
        """Save current config values before stress starts."""
        try:
            async with session.get(f"http://{host}/api/config") as resp:
                if resp.status == 200:
                    config = await resp.json()
                    self._saved_configs[host] = {
                        "brightness": config.get("brightness", 50),
                        "theme": config.get("theme", 0),
                        "color_brightness": config.get("color_brightness", 50),
                        "widget_style": config.get("widget_style", 0),
                    }
                    logger.info(f"Saved original config for {host}")
        except Exception as e:
            logger.warning(f"Failed to save config for {host}: {e}")

    async def restore_original_config(self, session: aiohttp.ClientSession,
                                       host: str):
        """Restore original config values."""
        saved = self._saved_configs.get(host)
        if not saved:
            logger.warning(f"No saved config for {host}")
            return
        try:
            for key, value in saved.items():
                endpoint = key.replace("_", "-")
                await session.post(
                    f"http://{host}/api/{endpoint}",
                    json={key: value},
                )
            logger.info(f"Restored original config for {host}")
        except Exception as e:
            logger.warning(f"Failed to restore config for {host}: {e}")

    async def _execute(self, session: aiohttp.ClientSession, host: str,
                       intensity: float):
        count = self._request_count.get(host, 0)
        self._request_count[host] = count + 1

        # Every 10th request: negative test
        if count % 10 == 9:
            await self._negative_test(session, host)
            return

        # Normal rotation
        action = count % 4
        start = time.monotonic()

        try:
            if action == 0:
                value = random.randint(0, 100)
                async with session.post(
                    f"http://{host}/api/brightness",
                    json={"brightness": value},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)
            elif action == 1:
                self._theme_cycle = (self._theme_cycle + 1) % 12
                async with session.post(
                    f"http://{host}/api/theme",
                    json={"theme": self._theme_cycle},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)
            elif action == 2:
                value = random.randint(0, 100)
                async with session.post(
                    f"http://{host}/api/color-brightness",
                    json={"color_brightness": value},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)
            elif action == 3:
                self._widget_cycle = (self._widget_cycle + 1) % 4
                async with session.post(
                    f"http://{host}/api/widget-style",
                    json={"widget_style": self._widget_cycle},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    self._record_result(host, elapsed_ms, resp.status)
        except Exception:
            raise

    async def _negative_test(self, session: aiohttp.ClientSession, host: str):
        """Send invalid data — device should reject with 400."""
        tests = [
            ("/api/brightness", {"brightness": 255}),
            ("/api/theme", {"theme": -1}),
            ("/api/theme", {"theme": 99}),
            ("/api/brightness", {}),
        ]

        endpoint, payload = random.choice(tests)
        start = time.monotonic()

        try:
            # Occasionally send malformed JSON
            if random.random() < 0.25:
                async with session.post(
                    f"http://{host}{endpoint}",
                    data="not json",
                    headers={"Content-Type": "application/json"},
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    if resp.status == 200:
                        logger.error(
                            f"VIOLATION: {host} accepted malformed JSON on {endpoint}"
                        )
                    self._record_result(host, elapsed_ms, resp.status)
            else:
                async with session.post(
                    f"http://{host}{endpoint}", json=payload,
                ) as resp:
                    elapsed_ms = (time.monotonic() - start) * 1000
                    if resp.status == 200:
                        logger.error(
                            f"VIOLATION: {host} accepted invalid data "
                            f"{payload} on {endpoint}"
                        )
                    self._record_result(host, elapsed_ms, resp.status)
        except Exception as e:
            self._record_error(host, str(e))
