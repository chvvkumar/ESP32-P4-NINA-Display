"""No-op metrics writer — InfluxDB removed."""


class InfluxDBWriter:
    """Stub writer that accepts all write() calls and discards them."""

    def __init__(self, *args, **kwargs):
        pass

    async def start(self):
        pass

    async def stop(self):
        pass

    def write(self, measurement, tags=None, fields=None, timestamp_ms=None):
        pass
