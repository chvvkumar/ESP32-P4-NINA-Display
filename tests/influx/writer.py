"""Batched InfluxDB line protocol writer."""
import asyncio
import time
import logging
import aiohttp
from collections import deque

logger = logging.getLogger(__name__)


class InfluxDBWriter:
    """Writes metrics to InfluxDB using the HTTP line protocol API."""

    def __init__(self, url: str, database: str, batch_size: int = 100,
                 flush_interval_s: float = 5.0):
        self.url = url.rstrip('/')
        self.database = database
        self.batch_size = batch_size
        self.flush_interval_s = flush_interval_s
        self._buffer: deque[str] = deque()
        self._session: aiohttp.ClientSession | None = None
        self._flush_task: asyncio.Task | None = None
        self._write_url = f"{self.url}/write?db={self.database}&precision=ms"

    async def start(self):
        """Start the writer and background flush task."""
        self._session = aiohttp.ClientSession()
        self._flush_task = asyncio.create_task(self._flush_loop())

    async def stop(self):
        """Flush remaining data and stop."""
        if self._flush_task:
            self._flush_task.cancel()
            try:
                await self._flush_task
            except asyncio.CancelledError:
                pass
        await self._flush()
        if self._session:
            await self._session.close()
            self._session = None

    def write(self, measurement: str, tags: dict[str, str],
              fields: dict[str, any], timestamp_ms: int | None = None):
        """Queue a data point for writing."""
        if timestamp_ms is None:
            timestamp_ms = int(time.time() * 1000)

        tag_str = ",".join(
            f"{k}={self._escape_tag(str(v))}"
            for k, v in sorted(tags.items()) if v
        )

        field_parts = []
        for k, v in sorted(fields.items()):
            if isinstance(v, bool):
                field_parts.append(f"{k}={'true' if v else 'false'}")
            elif isinstance(v, int):
                field_parts.append(f"{k}={v}i")
            elif isinstance(v, float):
                field_parts.append(f"{k}={v}")
            elif isinstance(v, str):
                field_parts.append(f'{k}="{self._escape_field_str(v)}"')

        if not field_parts:
            return

        field_str = ",".join(field_parts)
        if tag_str:
            line = f"{measurement},{tag_str} {field_str} {timestamp_ms}"
        else:
            line = f"{measurement} {field_str} {timestamp_ms}"

        self._buffer.append(line)

        if len(self._buffer) >= self.batch_size:
            try:
                loop = asyncio.get_running_loop()
                loop.call_soon(lambda: asyncio.ensure_future(self._flush()))
            except RuntimeError:
                pass

    async def _flush_loop(self):
        """Background flush at regular intervals."""
        while True:
            await asyncio.sleep(self.flush_interval_s)
            await self._flush()

    async def _flush(self):
        """Send buffered data to InfluxDB."""
        if not self._buffer or not self._session:
            return

        lines = []
        while self._buffer:
            try:
                lines.append(self._buffer.popleft())
            except IndexError:
                break

        if not lines:
            return

        body = "\n".join(lines)
        try:
            async with self._session.post(
                self._write_url, data=body,
                timeout=aiohttp.ClientTimeout(total=10)
            ) as resp:
                if resp.status not in (200, 204):
                    text = await resp.text()
                    logger.error(f"InfluxDB write failed ({resp.status}): {text}")
                    if len(self._buffer) < 10000:
                        for line in lines:
                            self._buffer.append(line)
        except Exception as e:
            logger.error(f"InfluxDB write error: {e}")

    @staticmethod
    def _escape_tag(s: str) -> str:
        return s.replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")

    @staticmethod
    def _escape_field_str(s: str) -> str:
        return s.replace("\\", "\\\\").replace('"', '\\"')
