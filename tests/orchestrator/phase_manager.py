"""Phase manager — drives the test plan as a state machine."""
import asyncio
import enum
import json
import logging
import os
import socket
import time

import aiohttp

logger = logging.getLogger(__name__)


class Phase(enum.Enum):
    STARTUP = "startup"
    STRESS = "stress"
    RECOVERY = "recovery"
    SOAK = "soak"


class PhaseManager:
    """State machine: STARTUP → STRESS → RECOVERY → SOAK (repeating)."""

    def __init__(self, config: dict, simulator, influx_writer):
        self.config = config
        self.simulator = simulator
        self.influx_writer = influx_writer

        self.current_phase = Phase.STARTUP
        self.phase_start_time: float = 0.0
        self.total_start_time: float = 0.0
        self.cycle_count: int = 0

        self._devices = config.get("devices", [])
        phases = config.get("phases", {})
        self._startup_duration_s = phases.get("startup_duration_s", 120)
        self._stress_duration_s = phases.get("stress_duration_s", 1800)
        self._recovery_duration_s = phases.get("recovery_duration_s", 300)
        self._soak_duration_s = phases.get("soak_duration_s", 82800)
        self._total_duration_days = phases.get("total_duration_days", 3)

        sim_cfg = config.get("simulator", {})
        self._sim_base_port = sim_cfg.get("base_port", 1888)

        os.makedirs("logs/saved_configs", exist_ok=True)
        os.makedirs("logs/violations", exist_ok=True)

    def get_current_phase(self) -> Phase:
        return self.current_phase

    def get_phase_elapsed_s(self) -> float:
        return time.monotonic() - self.phase_start_time

    def get_total_elapsed_s(self) -> float:
        if self.total_start_time == 0.0:
            return 0.0
        return time.monotonic() - self.total_start_time

    def _enter_phase(self, phase: Phase):
        self.current_phase = phase
        self.phase_start_time = time.monotonic()
        logger.info(f"Phase → {phase.value}")
        self.influx_writer.write(
            "test_phase", tags={},
            fields={
                "phase_name": phase.value,
                "phase_start_epoch": time.time(),
                "elapsed_s": self.get_total_elapsed_s(),
            },
        )
        self.influx_writer.write(
            "test_annotation",
            tags={"type": "phase_change"},
            fields={"description": f"Phase: {phase.value}"},
        )

    async def run(self, workload_manager, metrics_collector, alert_monitor):
        """Execute the full test plan."""
        self.total_start_time = time.monotonic()
        total_duration_s = self._total_duration_days * 86400

        # ── STARTUP ──
        self._enter_phase(Phase.STARTUP)
        await self._run_startup(metrics_collector)

        # ── Cycle: STRESS → RECOVERY → SOAK ──
        while self.get_total_elapsed_s() < total_duration_s:
            self.cycle_count += 1
            logger.info(f"Starting test cycle {self.cycle_count}")

            # STRESS
            self._enter_phase(Phase.STRESS)
            await workload_manager.start_all("stress")
            await self._wait_phase(self._stress_duration_s, total_duration_s)
            await workload_manager.stop_all()

            if self.get_total_elapsed_s() >= total_duration_s:
                break

            # RECOVERY
            self._enter_phase(Phase.RECOVERY)
            await self._run_recovery()
            await self._wait_phase(self._recovery_duration_s, total_duration_s)

            if self.get_total_elapsed_s() >= total_duration_s:
                break

            # SOAK
            self._enter_phase(Phase.SOAK)
            await workload_manager.start_all("soak")
            await self._run_soak(workload_manager)
            await workload_manager.stop_all()

        logger.info(f"Test complete after {self.cycle_count} cycles")

    async def _run_startup(self, metrics_collector):
        """STARTUP phase: verify devices, save configs, configure for test."""
        from influx.setup import ensure_database

        influx_cfg = self.config.get("influxdb", {})

        # 1. Ensure InfluxDB database
        await ensure_database(influx_cfg["url"], influx_cfg["database"])

        # 2. Start simulator
        await self.simulator.start()

        # 3. Verify all devices reachable
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=10)
        ) as session:
            for device in self._devices:
                host = device["host"]
                reachable = False
                for attempt in range(3):
                    try:
                        async with session.get(
                            f"http://{host}/api/version"
                        ) as resp:
                            if resp.status == 200:
                                reachable = True
                                logger.info(f"Device {host} reachable")
                                break
                    except Exception as e:
                        logger.warning(
                            f"Device {host} attempt {attempt + 1}: {e}"
                        )
                    await asyncio.sleep(10)
                if not reachable:
                    raise RuntimeError(
                        f"Device {host} unreachable after 3 attempts"
                    )

            # 4. Save device configs
            for device in self._devices:
                host = device["host"]
                try:
                    async with session.get(
                        f"http://{host}/api/config"
                    ) as resp:
                        cfg = await resp.json()
                    path = f"logs/saved_configs/{host}.json"
                    with open(path, "w") as f:
                        json.dump(cfg, f, indent=2)
                    logger.info(f"Saved config for {host}")
                except Exception as e:
                    logger.error(f"Failed to save config for {host}: {e}")

            # 5-6. Disable Spotify/AllSky, set NINA URLs to simulator
            pi_ip = self._get_local_ip()
            for device in self._devices:
                host = device["host"]
                payload = {
                    "spotify_enabled": False,
                    "allsky_enabled": False,
                    "api_url_0": f"http://{pi_ip}:{self._sim_base_port}",
                    "api_url_1": f"http://{pi_ip}:{self._sim_base_port + 1}",
                    "api_url_2": f"http://{pi_ip}:{self._sim_base_port + 2}",
                    "instance_enabled_0": True,
                    "instance_enabled_1": True,
                    "instance_enabled_2": True,
                }
                try:
                    async with session.post(
                        f"http://{host}/api/config", json=payload
                    ) as resp:
                        if resp.status == 200:
                            logger.info(f"Configured {host} for test")
                        else:
                            text = await resp.text()
                            logger.error(
                                f"Config push to {host} failed ({resp.status}): {text}"
                            )
                except Exception as e:
                    logger.error(f"Failed to configure {host}: {e}")

            # 6.5. Reboot all devices so they pick up new NINA instance URLs
            for device in self._devices:
                host = device["host"]
                try:
                    async with session.post(
                        f"http://{host}/api/reboot"
                    ) as resp:
                        logger.info(f"Rebooting {host} (status {resp.status})")
                except Exception:
                    pass  # Connection will drop during reboot

            # Wait for devices to come back online
            logger.info("Waiting 15s for devices to reboot...")
            await asyncio.sleep(15)
            for device in self._devices:
                host = device["host"]
                back_online = False
                for attempt in range(12):  # Up to 60s
                    try:
                        async with session.get(
                            f"http://{host}/api/version"
                        ) as resp:
                            if resp.status == 200:
                                back_online = True
                                logger.info(f"{host} back online")
                                break
                    except Exception:
                        pass
                    await asyncio.sleep(5)
                if not back_online:
                    logger.error(f"{host} did not come back after reboot")

            # Brief settle time before starting tests
            logger.info("Waiting 10s for devices to settle...")
            await asyncio.sleep(10)

            # 7. Wait for all 9 connections
            timeout_s = self.config.get("thresholds", {}).get(
                "nina_connect_timeout_s", 60
            )
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                all_connected = True
                for device in self._devices:
                    try:
                        async with session.get(
                            f"http://{device['host']}/api/nina/status"
                        ) as resp:
                            if resp.status == 200:
                                data = await resp.json()
                                for inst in data.get("instances", []):
                                    if inst.get("connection_state") != "connected":
                                        all_connected = False
                    except Exception:
                        all_connected = False
                if all_connected:
                    logger.info("All 9 NINA connections established")
                    break
                await asyncio.sleep(5)
            else:
                logger.warning("Timed out waiting for NINA connections")

        # 8. Start metrics
        await metrics_collector.start()
        logger.info("STARTUP complete")

    async def _run_recovery(self):
        """Restore saved display settings during RECOVERY."""
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=10)
        ) as session:
            for device in self._devices:
                host = device["host"]
                cfg_path = f"logs/saved_configs/{host}.json"
                if not os.path.exists(cfg_path):
                    continue
                try:
                    with open(cfg_path) as f:
                        saved = json.load(f)
                    restore = {}
                    for key in ("brightness", "theme", "widget_style",
                                "color_brightness"):
                        if key in saved:
                            restore[key] = saved[key]
                    if restore:
                        async with session.post(
                            f"http://{host}/api/config", json=restore
                        ) as resp:
                            if resp.status == 200:
                                logger.info(f"Restored settings on {host}")
                except Exception as e:
                    logger.error(f"Failed to restore {host}: {e}")

    async def _run_soak(self, workload_manager):
        """Soak phase with hourly mini stress bursts."""
        elapsed = 0.0
        hour_counter = 0.0
        burst_interval = 3600.0
        burst_duration = 120.0
        total_limit = self._total_duration_days * 86400

        while elapsed < self._soak_duration_s:
            if self.get_total_elapsed_s() >= total_limit:
                break

            step = min(10.0, self._soak_duration_s - elapsed)
            await asyncio.sleep(step)
            elapsed += step
            hour_counter += step

            if hour_counter >= burst_interval:
                hour_counter = 0.0
                logger.info("Mini stress burst during soak")
                self.influx_writer.write(
                    "test_annotation",
                    tags={"type": "mini_burst"},
                    fields={"description": "Mini stress burst (50% intensity)"},
                )
                await workload_manager.stop_all()
                await workload_manager.start_all("mini_burst")
                await asyncio.sleep(burst_duration)
                await workload_manager.stop_all()
                await workload_manager.start_all("soak")

    async def _wait_phase(self, duration_s: float, total_limit_s: float):
        """Wait for a phase to complete, respecting total time limit."""
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            if self.get_total_elapsed_s() >= total_limit_s:
                break
            await asyncio.sleep(5)

    @staticmethod
    def _get_local_ip() -> str:
        """Get the local IP address for simulator URLs."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"
