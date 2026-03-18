"""NINA API Simulator — serves REST + WebSocket endpoints for 3 instances."""
import asyncio
import base64
import json
import logging
import os
from aiohttp import web, WSMsgType

from .session_timeline import SessionTimeline

logger = logging.getLogger(__name__)

# Minimal 2x2 gray JPEG for prepared-image endpoint
_TEST_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRof"
    "Hh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwh"
    "MjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAAR"
    "CAABAAEDASIAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAA"
    "AAD/xAAUAQEAAAAAAAAAAAAAAAAAAAAA/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMR"
    "AD8AKwA//9k="
)


def _wrap(data):
    """Wrap response data in ninaAPI standard envelope."""
    return {"Response": data, "Error": "", "StatusCode": 200, "Success": True, "Type": "API"}


class NinaSimulatorServer:
    """Runs 3 NINA API simulator instances on consecutive ports."""

    def __init__(self, bind_address: str = "0.0.0.0", base_port: int = 1888,
                 speed_profile: str = "normal"):
        self.bind_address = bind_address
        self.base_port = base_port
        self.timelines = [SessionTimeline(i, speed_profile=speed_profile) for i in range(3)]
        self._ws_clients: list[set[web.WebSocketResponse]] = [set(), set(), set()]
        self._runners: list[web.AppRunner] = []
        self._advance_task: asyncio.Task | None = None
        self._running = False

    async def start(self):
        """Start all 3 simulator instances."""
        for i in range(3):
            app = self._create_app(i)
            runner = web.AppRunner(app)
            await runner.setup()
            site = web.TCPSite(runner, self.bind_address, self.base_port + i)
            await site.start()
            self._runners.append(runner)
            logger.info(f"Simulator instance {i} listening on "
                        f"{self.bind_address}:{self.base_port + i}")

        self._running = True
        self._advance_task = asyncio.create_task(self._advance_loop())

    async def stop(self):
        """Stop all simulator instances."""
        self._running = False
        if self._advance_task:
            self._advance_task.cancel()
            try:
                await self._advance_task
            except asyncio.CancelledError:
                pass

        # Close all WebSocket connections
        for client_set in self._ws_clients:
            for ws in list(client_set):
                await ws.close()
            client_set.clear()

        for runner in self._runners:
            await runner.cleanup()
        self._runners.clear()
        logger.info("Simulator stopped")

    def set_speed_profile(self, name: str):
        """Switch all simulator instances to a new speed profile."""
        for tl in self.timelines:
            tl.set_speed_profile(name)
        logger.info(f"Simulator speed profile → {name}")

    def _create_app(self, instance_idx: int) -> web.Application:
        """Create an aiohttp app for one simulator instance."""
        app = web.Application()
        tl = self.timelines[instance_idx]

        async def camera_info(req):
            return web.json_response(_wrap(tl.get_camera_info()))

        async def guider_info(req):
            return web.json_response(_wrap(tl.get_guider_info()))

        async def focuser_info(req):
            return web.json_response(_wrap(tl.get_focuser_info()))

        async def mount_info(req):
            return web.json_response(_wrap(tl.get_mount_info()))

        async def filter_info(req):
            return web.json_response(_wrap(tl.get_filter_info()))

        async def switch_info(req):
            return web.json_response(_wrap(tl.get_switch_info()))

        async def safety_info(req):
            return web.json_response(_wrap(tl.get_safety_info()))

        async def equipment_info(req):
            return web.json_response(_wrap(tl.get_equipment_info()))

        async def profile_show(req):
            return web.json_response(_wrap(tl.get_profile()))

        async def sequence_json(req):
            return web.json_response(_wrap(tl.get_sequence_json()))

        async def image_history(req):
            count_only = req.query.get("count", "").lower() == "true"
            result = tl.get_image_history(count_only=count_only)
            return web.json_response(_wrap(result))

        async def prepared_image(req):
            tl.requests_served += 1
            return web.Response(body=_TEST_JPEG, content_type="image/jpeg")

        async def websocket_handler(req):
            ws = web.WebSocketResponse()
            await ws.prepare(req)
            self._ws_clients[instance_idx].add(ws)
            tl.ws_connections += 1
            logger.debug(f"WebSocket connected to instance {instance_idx}")

            try:
                async for msg in ws:
                    if msg.type == WSMsgType.TEXT:
                        pass  # Clients don't send meaningful data
                    elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                        break
            finally:
                self._ws_clients[instance_idx].discard(ws)
                tl.ws_connections = max(0, tl.ws_connections - 1)
                logger.debug(f"WebSocket disconnected from instance {instance_idx}")

            return ws

        app.router.add_get("/v2/api/equipment/camera/info", camera_info)
        app.router.add_get("/v2/api/equipment/guider/info", guider_info)
        app.router.add_get("/v2/api/equipment/focuser/info", focuser_info)
        app.router.add_get("/v2/api/equipment/mount/info", mount_info)
        app.router.add_get("/v2/api/equipment/filterwheel/info", filter_info)
        app.router.add_get("/v2/api/equipment/switch/info", switch_info)
        app.router.add_get("/v2/api/equipment/safetymonitor/info", safety_info)
        app.router.add_get("/v2/api/equipment/info", equipment_info)
        app.router.add_get("/v2/api/profile/show", profile_show)
        app.router.add_get("/v2/api/sequence/json", sequence_json)
        app.router.add_get("/v2/api/image-history", image_history)
        app.router.add_get("/v2/api/prepared-image", prepared_image)
        app.router.add_get("/v2/socket", websocket_handler)

        return app

    async def _advance_loop(self):
        """Advance all timelines every second and broadcast events."""
        while self._running:
            for i, tl in enumerate(self.timelines):
                events = tl.advance(1.0)
                if events:
                    await self._broadcast_events(i, events)
            await asyncio.sleep(1.0)

    async def _broadcast_events(self, instance_idx: int, events: list[dict]):
        """Send events to all connected WebSocket clients for an instance."""
        dead = set()
        for ws in self._ws_clients[instance_idx]:
            for event in events:
                try:
                    await ws.send_json(event)
                except Exception:
                    dead.add(ws)
                    break
        self._ws_clients[instance_idx] -= dead

    def get_stats(self, instance_idx: int) -> dict:
        """Get stats for a simulator instance."""
        return self.timelines[instance_idx].get_stats()
