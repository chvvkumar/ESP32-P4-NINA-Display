#!/usr/bin/env python3
"""
ESP32-P4 NINA Display — Automated Stress & Soak Test Suite

Usage:
    python run_test.py                          # All devices, default 3-day test
    python run_test.py --device ninadash2.lan   # Single device by hostname
    python run_test.py --device 2               # Single device by number (1-indexed)
    python run_test.py --device all             # Explicit all (default)
    python run_test.py --duration-days 7        # Custom duration
    python run_test.py --stress-only --duration-hours 2  # Stress only
    python run_test.py --restore-only           # Restore device configs after crash
"""
import argparse
import asyncio
import json
import logging
import os
import signal
import sys
import time
from datetime import datetime

import yaml
import aiohttp

from influx.writer import InfluxDBWriter
from simulator.nina_server import NinaSimulatorServer
from orchestrator.phase_manager import PhaseManager
from orchestrator.metrics_collector import MetricsCollector
from orchestrator.alert_monitor import AlertMonitor
from orchestrator.report_generator import ReportGenerator
from orchestrator.control_api import ControlAPI
from workloads.http_stress import HttpStressWorkload
from workloads.config_churn import ConfigChurnWorkload
from workloads.page_cycle import PageCycleWorkload
from workloads.concurrent_conns import ConcurrentConnsWorkload
from workloads.nina_monitor import NinaMonitorWorkload

logger = logging.getLogger("ninadash_test")


class WorkloadManager:
    """Manages all workloads and their lifecycle."""

    def __init__(self, config: dict, devices: list[dict], influx_writer):
        self.config = config
        self.devices = devices
        self.influx_writer = influx_writer
        self._lvgl_stress = config.get("lvgl_stress", False)

        wl_cfg = config.get("workloads", {})

        self.http_stress = HttpStressWorkload(devices, influx_writer)
        self.config_churn = ConfigChurnWorkload(devices, influx_writer)
        self.page_cycle = PageCycleWorkload(
            devices, influx_writer,
            dwell_s=wl_cfg.get("page_cycle", {}).get("dwell_s", 2),
            include_summary=wl_cfg.get("page_cycle", {}).get("include_summary", False),
        )
        self.concurrent_conns = ConcurrentConnsWorkload(
            devices, influx_writer,
            endpoint=wl_cfg.get("concurrent_conns", {}).get(
                "endpoint", "/api/perf"
            ),
        )
        self.nina_monitor = NinaMonitorWorkload(devices, influx_writer)

        self._all = [
            self.http_stress,
            self.config_churn,
            self.page_cycle,
            self.concurrent_conns,
            self.nina_monitor,
        ]
        self._wl_cfg = wl_cfg

    async def start_all(self, mode: str):
        """Start all workloads at the given mode intensity."""
        cfg = self._wl_cfg

        if mode == "stress":
            await self.http_stress.start(
                cfg.get("http_stress", {}).get("stress_rps", 20)
            )
            if self._lvgl_stress:
                await self.config_churn.start(
                    cfg.get("config_churn", {}).get("stress_rps", 5)
                )
            await self.page_cycle.start(
                cfg.get("page_cycle", {}).get("stress_interval_s", 2)
            )
            await self.concurrent_conns.start(
                cfg.get("concurrent_conns", {}).get("stress_count", 10)
            )
            await self.nina_monitor.start(0.1)  # Fixed 10s interval
        elif mode == "soak":
            await self.http_stress.start(
                cfg.get("http_stress", {}).get("soak_rps", 0.033)
            )
            await self.config_churn.start(
                cfg.get("config_churn", {}).get("soak_rps", 0.017)
            )
            await self.page_cycle.start(
                cfg.get("page_cycle", {}).get("soak_interval_s", 30)
            )
            await self.concurrent_conns.start(
                cfg.get("concurrent_conns", {}).get("soak_count", 2)
            )
            await self.nina_monitor.start(0.1)
        elif mode == "mini_burst":
            # 50% of stress intensity
            await self.http_stress.start(
                cfg.get("http_stress", {}).get("stress_rps", 20) * 0.5
            )
            if self._lvgl_stress:
                await self.config_churn.start(
                    cfg.get("config_churn", {}).get("stress_rps", 5) * 0.5
                )
            await self.page_cycle.start(
                cfg.get("page_cycle", {}).get("stress_interval_s", 2) * 2
            )
            await self.concurrent_conns.start(
                cfg.get("concurrent_conns", {}).get("stress_count", 10) // 2
            )
            await self.nina_monitor.start(0.1)

        logger.info(f"All workloads started in '{mode}' mode")

    async def stop_all(self):
        """Stop all workloads."""
        for wl in self._all:
            await wl.stop()
        logger.info("All workloads stopped")

    async def save_configs(self, session: aiohttp.ClientSession):
        """Save original configs via config_churn workload."""
        for device in self.devices:
            await self.config_churn.save_original_config(
                session, device["host"]
            )

    async def restore_configs(self, session: aiohttp.ClientSession):
        """Restore original configs via config_churn workload."""
        for device in self.devices:
            await self.config_churn.restore_original_config(
                session, device["host"]
            )

    async def set_soak_display(self, session: aiohttp.ClientSession):
        """Set fixed display settings for soak phase."""
        for device in self.devices:
            host = device["host"]
            try:
                await session.post(f"http://{host}/api/brightness",
                                   json={"brightness": 50})
                await session.post(f"http://{host}/api/color-brightness",
                                   json={"color_brightness": 100})
                await session.post(f"http://{host}/api/theme",
                                   json={"theme": 0})
                await session.post(f"http://{host}/api/widget-style",
                                   json={"widget_style": 0})
                logger.info(f"Set soak display settings for {host}")
            except Exception as e:
                logger.warning(f"Failed to set soak display for {host}: {e}")


def setup_logging(log_level: str):
    """Configure logging to console and file."""
    os.makedirs("logs", exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = f"logs/test_{timestamp}.log"

    root_logger = logging.getLogger()
    root_logger.setLevel(getattr(logging, log_level.upper(), logging.INFO))

    # Console handler
    console = logging.StreamHandler(sys.stdout)
    console.setLevel(logging.INFO)
    console.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)-7s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    ))
    root_logger.addHandler(console)

    # File handler
    file_handler = logging.FileHandler(log_file)
    file_handler.setLevel(logging.DEBUG)
    file_handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)-7s] %(name)s: %(message)s",
    ))
    root_logger.addHandler(file_handler)

    logger.info(f"Logging to {log_file}")


async def restore_only(config: dict):
    """Restore device configs from saved files (crash recovery)."""
    saved_dir = os.path.join("logs", "saved_configs")
    if not os.path.exists(saved_dir):
        logger.error(f"No saved configs found in {saved_dir}")
        return

    async with aiohttp.ClientSession(
        timeout=aiohttp.ClientTimeout(total=15)
    ) as session:
        for device in config["devices"]:
            host = device["host"]
            path = os.path.join(saved_dir, f"{host}.json")
            if not os.path.exists(path):
                logger.warning(f"No saved config for {host}")
                continue
            try:
                with open(path) as f:
                    saved_config = json.load(f)
                async with session.post(
                    f"http://{host}/api/config", json=saved_config
                ) as resp:
                    if resp.status == 200:
                        logger.info(f"Restored config for {host}")
                    else:
                        text = await resp.text()
                        logger.error(f"Restore failed for {host}: {text}")
            except Exception as e:
                logger.error(f"Failed to restore {host}: {e}")


async def main():
    parser = argparse.ArgumentParser(
        description="ESP32-P4 NINA Display Stress & Soak Test Suite"
    )
    parser.add_argument(
        "--config", default="test_config.yaml",
        help="Path to test config YAML (default: test_config.yaml)",
    )
    parser.add_argument(
        "--duration-days", type=float, default=None,
        help="Override total test duration in days",
    )
    parser.add_argument(
        "--duration-hours", type=float, default=None,
        help="Override total test duration in hours",
    )
    parser.add_argument(
        "--stress-only", action="store_true",
        help="Run stress phase only (no soak)",
    )
    parser.add_argument(
        "--restore-only", action="store_true",
        help="Restore saved device configs and exit",
    )
    parser.add_argument(
        "--device", default="all",
        help=(
            "Device(s) to test: hostname (e.g. ninadash2.lan), "
            "1-indexed number (e.g. 2), or 'all' (default: all)"
        ),
    )
    parser.add_argument(
        "--log-level", default="INFO",
        help="Logging level (default: INFO)",
    )
    parser.add_argument(
        "--api-port", type=int, default=8880,
        help="Control API port (default: 8880)",
    )

    args = parser.parse_args()
    setup_logging(args.log_level)

    # Load config
    with open(args.config) as f:
        config = yaml.safe_load(f)

    # Filter devices
    if args.device.lower() != "all":
        all_devices = config["devices"]
        choice = args.device
        matched = None
        # Try as 1-indexed number
        if choice.isdigit():
            idx = int(choice) - 1
            if 0 <= idx < len(all_devices):
                matched = [all_devices[idx]]
            else:
                parser.error(
                    f"Device number {choice} out of range "
                    f"(1-{len(all_devices)})"
                )
        else:
            # Try as hostname (exact or partial match)
            matched = [d for d in all_devices if choice in d["host"]]
            if not matched:
                hosts = ", ".join(d["host"] for d in all_devices)
                parser.error(
                    f"No device matching '{choice}'. "
                    f"Available: {hosts}"
                )
        config["devices"] = matched
        logger.info(
            f"Testing device(s): "
            f"{', '.join(d['host'] for d in matched)}"
        )
    else:
        logger.info(
            f"Testing all devices: "
            f"{', '.join(d['host'] for d in config['devices'])}"
        )

    # Apply duration overrides
    if args.duration_days is not None:
        config["phases"]["total_duration_days"] = args.duration_days
    elif args.duration_hours is not None:
        config["phases"]["total_duration_days"] = args.duration_hours / 24.0

    if args.stress_only:
        config["phases"]["soak_duration_s"] = 0
        config["phases"]["total_duration_days"] = (
            config["phases"]["stress_duration_s"] +
            config["phases"]["recovery_duration_s"]
        ) / 86400.0

    # Restore-only mode
    if args.restore_only:
        await restore_only(config)
        return

    # ── Initialize components ──
    influx_writer = InfluxDBWriter()

    sim_cfg = config["simulator"]
    simulator = NinaSimulatorServer(
        bind_address=sim_cfg.get("bind_address", "0.0.0.0"),
        base_port=sim_cfg.get("base_port", 1888),
        speed_profile=sim_cfg.get("speed_profiles", {}).get("stress", "fast"),
    )

    phase_manager = PhaseManager(config, simulator, influx_writer)
    metrics_collector = MetricsCollector(
        config["devices"], influx_writer, phase_manager
    )
    alert_monitor = AlertMonitor(
        config["thresholds"], metrics_collector, influx_writer, phase_manager
    )
    workload_manager = WorkloadManager(
        config, config["devices"], influx_writer
    )
    report_generator = ReportGenerator(
        alert_monitor, phase_manager, metrics_collector
    )

    # ── Signal handling for graceful shutdown ──
    shutdown_event = asyncio.Event()

    def handle_signal():
        logger.info("Shutdown signal received")
        shutdown_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, handle_signal)
        except NotImplementedError:
            # Windows doesn't support add_signal_handler
            pass

    # ── Control API ──
    control_api = ControlAPI(
        phase_manager, workload_manager, metrics_collector,
        alert_monitor, report_generator, influx_writer, simulator,
        config, port=args.api_port,
    )

    # ── Run ──
    try:
        await alert_monitor.start()

        # Start control API
        await control_api.start(shutdown_event)

        # Save original workload configs
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=15)
        ) as session:
            await workload_manager.save_configs(session)

        # Run the phase manager (main orchestration)
        test_task = asyncio.create_task(
            phase_manager.run(
                workload_manager, metrics_collector, alert_monitor
            )
        )

        # Wait for either completion or shutdown signal
        done, pending = await asyncio.wait(
            [test_task, asyncio.create_task(shutdown_event.wait())],
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()

    except Exception as e:
        logger.error(f"Test failed with error: {e}", exc_info=True)
    finally:
        # ── Teardown ──
        logger.info("Starting teardown...")

        # 1. Stop workloads
        await workload_manager.stop_all()

        # 2. Restore device configs
        async with aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=15)
        ) as session:
            await workload_manager.restore_configs(session)

        # 3. Stop alert monitor and metrics collector
        await alert_monitor.stop()
        await metrics_collector.stop()

        # 4. Stop simulator (keep control API alive for result retrieval)
        await simulator.stop()

        # 5. Generate report (also saves to logs/report_{timestamp}.txt)
        report = report_generator.generate()
        print("\n" + report)

        # 6. Mark test as completed so control API serves final state
        control_api.mark_completed(report)
        logger.info(
            "Test complete — control API still running on port "
            f"{args.api_port} for result retrieval. "
            "POST /api/stop or Ctrl+C to shut down."
        )

        # 7. Wait for explicit shutdown (POST /api/stop or second signal)
        final_shutdown = asyncio.Event()

        def handle_final_signal():
            logger.info("Final shutdown signal received")
            final_shutdown.set()

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, handle_final_signal)
            except NotImplementedError:
                pass

        control_api.set_shutdown_event(final_shutdown)
        await final_shutdown.wait()

        # 8. Now stop control API
        await control_api.stop()
        logger.info("Control API stopped")

        # 9. Final result
        if alert_monitor.has_critical_violations():
            logger.error(
                f"TEST FAILED — {alert_monitor.get_critical_count()} "
                f"critical violations"
            )
            sys.exit(1)
        else:
            logger.info("TEST PASSED — no critical violations")


if __name__ == "__main__":
    asyncio.run(main())
