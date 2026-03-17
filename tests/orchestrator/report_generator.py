"""Report generator -- final pass/fail report."""
import logging
import os
import time
from datetime import datetime, timezone

logger = logging.getLogger(__name__)


class ReportGenerator:
    """Generates a text report summarizing the test run."""

    def __init__(self, alert_monitor, phase_manager, metrics_collector):
        self.alert_monitor = alert_monitor
        self.phase_manager = phase_manager
        self.metrics_collector = metrics_collector

    def generate(self, output_path: str | None = None) -> str:
        """Generate the final test report.

        Returns the report text and writes it to a file.
        """
        os.makedirs("logs", exist_ok=True)

        violations = self.alert_monitor.get_violations()
        critical_count = self.alert_monitor.get_critical_count()
        has_critical = self.alert_monitor.has_critical_violations()
        total_elapsed = self.phase_manager.get_total_elapsed_s()
        cycles = self.phase_manager.cycle_count
        devices = self.metrics_collector.devices

        passed = not has_critical
        verdict = "PASS" if passed else "FAIL"

        now = datetime.now(timezone.utc)
        lines = []

        # -- Header --
        lines.append("=" * 72)
        lines.append("  ESP32-P4 NINA Display -- Endurance Test Report")
        lines.append("=" * 72)
        lines.append("")
        lines.append(f"  Verdict:        {verdict}")
        lines.append(f"  Generated:      {now.strftime('%Y-%m-%d %H:%M:%S UTC')}")
        lines.append(f"  Total Duration:  {self._format_duration(total_elapsed)}")
        lines.append(f"  Cycles:         {cycles}")
        lines.append(f"  Final Phase:    {self.phase_manager.get_current_phase().value}")
        lines.append(f"  Devices:        {len(devices)}")
        lines.append("")

        # -- Violation Summary --
        lines.append("-" * 72)
        lines.append("  Violation Summary")
        lines.append("-" * 72)
        lines.append(f"  Total violations:    {len(violations)}")
        lines.append(f"  Critical:            {critical_count}")

        high_count = sum(1 for v in violations if v.severity.value == "HIGH")
        medium_count = sum(1 for v in violations if v.severity.value == "MEDIUM")
        lines.append(f"  High:                {high_count}")
        lines.append(f"  Medium:              {medium_count}")
        lines.append("")

        # -- Violation Timeline --
        if violations:
            lines.append("-" * 72)
            lines.append("  Violation Timeline")
            lines.append("-" * 72)
            for v in violations:
                ts = datetime.fromtimestamp(v.timestamp, tz=timezone.utc)
                ts_str = ts.strftime("%Y-%m-%d %H:%M:%S")
                lines.append(
                    f"  [{ts_str}] [{v.severity.value:8s}] "
                    f"{v.device}: {v.check_name}"
                )
                lines.append(f"    {v.message}")
                if v.value or v.threshold:
                    lines.append(
                        f"    value={v.value}, threshold={v.threshold}"
                    )
            lines.append("")

        # -- Fleet Overview --
        lines.append("-" * 72)
        lines.append("  Fleet Overview")
        lines.append("-" * 72)
        all_metrics = []
        for device in devices:
            m = self.metrics_collector.get_latest_metrics(device["host"])
            if m:
                all_metrics.append(m)
        if all_metrics:
            total_http_req = sum(
                m.get("http", {}).get("request_count", 0) for m in all_metrics
            )
            total_http_fail = sum(
                m.get("http", {}).get("failure_count", 0) for m in all_metrics
            )
            avg_heap = sum(
                m.get("heap_free", 0) for m in all_metrics
            ) // len(all_metrics)
            min_heap_min = min(
                (m.get("heap_min", float("inf")) for m in all_metrics),
                default=0,
            )
            avg_psram = sum(
                m.get("psram_free", 0) for m in all_metrics
            ) // len(all_metrics)
            min_psram_min = min(
                (m.get("psram_min", float("inf")) for m in all_metrics),
                default=0,
            )
            fail_pct = (
                (total_http_fail / total_http_req * 100)
                if total_http_req > 0
                else 0.0
            )
            lines.append(
                f"  HTTP requests:     {total_http_req:,} total, "
                f"{total_http_fail:,} failed ({fail_pct:.1f}%)"
            )
            lines.append(
                f"  Avg heap free:     {avg_heap:,}  "
                f"Min heap_min:  {min_heap_min:,}"
            )
            lines.append(
                f"  Avg PSRAM free:    {avg_psram:,}  "
                f"Min psram_min: {min_psram_min:,}"
            )
        else:
            lines.append("  No metrics available from any device")
        lines.append("")

        # -- Per-Device Summary --
        lines.append("-" * 72)
        lines.append("  Per-Device Summary")
        lines.append("-" * 72)
        for device in devices:
            host = device["host"]
            name = device.get("name", host)
            metrics = self.metrics_collector.get_latest_metrics(host)
            dev_violations = [v for v in violations if v.device == host]

            lines.append(f"  {name} ({host})")
            lines.append(f"    Violations: {len(dev_violations)}")

            if not metrics:
                lines.append("    No metrics available")
                lines.append("")
                continue

            # Memory
            lines.append("")
            lines.append("    Memory:")
            lines.append(
                f"      Heap:   {metrics.get('heap_free', 0):,} free"
                f" / {metrics.get('heap_min', 0):,} min"
                f" / {metrics.get('heap_largest_free_block', 0):,} largest block"
            )
            lines.append(
                f"      PSRAM:  {metrics.get('psram_free', 0):,} free"
                f" / {metrics.get('psram_min', 0):,} min"
                f" / {metrics.get('psram_largest_free_block', 0):,} largest block"
            )

            # CPU
            lines.append("")
            lines.append("    CPU:")
            core0 = metrics.get("core0_load")
            core1 = metrics.get("core1_load")
            total = metrics.get("total_load")
            lines.append(
                f"      Core 0: {self._fmt_pct(core0)}  "
                f"Core 1: {self._fmt_pct(core1)}  "
                f"Total: {self._fmt_pct(total)}"
            )
            lines.append(
                f"      Tasks: {metrics.get('task_count', 'N/A')}"
            )

            # Network
            http = metrics.get("http", {})
            http_req = http.get("request_count", 0)
            http_fail = http.get("failure_count", 0)
            http_retry = http.get("retry_count", 0)
            http_fail_pct = (
                (http_fail / http_req * 100) if http_req > 0 else 0.0
            )
            lines.append("")
            lines.append("    Network:")
            lines.append(
                f"      HTTP requests: {http_req:,} total, "
                f"{http_fail:,} failed ({http_fail_pct:.1f}%), "
                f"{http_retry:,} retries"
            )
            lines.append(
                f"      HTTP latency:  avg {http.get('avg_ms', 0):.0f}ms, "
                f"max {http.get('max_ms', 0):.0f}ms"
            )
            rssi = metrics.get("rssi")
            rssi_avg = metrics.get("rssi_avg")
            wifi_disc = metrics.get("wifi_disconnect_count", 0)
            rssi_str = f"{rssi} dBm" if rssi is not None else "N/A"
            rssi_avg_str = f"avg {rssi_avg}" if rssi_avg is not None else ""
            lines.append(
                f"      WiFi RSSI:     {rssi_str}"
                f"{' (' + rssi_avg_str + ')' if rssi_avg_str else ''}"
                f", disconnects: {wifi_disc}"
            )

            # UI Performance
            lvgl_avg = metrics.get("lvgl_render_avg_ms")
            lock_avg = metrics.get("ui_lock_wait_avg_ms")
            lock_max = metrics.get("ui_lock_wait_max_ms")
            poll_avg = metrics.get("poll_cycle_avg_ms")
            poll_max = metrics.get("poll_cycle_max_ms")
            if any(v is not None for v in [lvgl_avg, lock_avg, poll_avg]):
                lines.append("")
                lines.append("    UI Performance:")
                if lvgl_avg is not None:
                    lines.append(
                        f"      LVGL render:   avg {lvgl_avg:.0f}ms"
                    )
                if lock_avg is not None or lock_max is not None:
                    parts = []
                    if lock_avg is not None:
                        parts.append(f"avg {lock_avg:.0f}ms")
                    if lock_max is not None:
                        parts.append(f"max {lock_max:.0f}ms")
                    lines.append(
                        f"      Lock wait:     {', '.join(parts)}"
                    )
                if poll_avg is not None or poll_max is not None:
                    parts = []
                    if poll_avg is not None:
                        parts.append(f"avg {poll_avg:.0f}ms")
                    if poll_max is not None:
                        parts.append(f"max {poll_max:.0f}ms")
                    lines.append(
                        f"      Poll cycle:    {', '.join(parts)}"
                    )

            # Endpoint Latencies
            ep_latencies = metrics.get("endpoints", {})
            raw_perf = metrics.get("_raw_perf", {})
            raw_endpoints = raw_perf.get("endpoints", {})
            ep_with_data = {}
            for ep_name, avg_ms in ep_latencies.items():
                if avg_ms is not None and avg_ms > 0:
                    count = raw_endpoints.get(ep_name, {}).get("count", 0)
                    ep_with_data[ep_name] = (avg_ms, count)
            if ep_with_data:
                lines.append("")
                lines.append("    Endpoint Latencies:")
                # Sort by avg_ms descending
                for ep_name, (avg_ms, count) in sorted(
                    ep_with_data.items(), key=lambda x: x[1][0], reverse=True
                ):
                    count_str = f" ({count:,} calls)" if count > 0 else ""
                    lines.append(
                        f"      {ep_name + ':':25s} avg {avg_ms:.0f}ms{count_str}"
                    )

            # Top Tasks by CPU
            top_tasks = metrics.get("tasks", [])
            if top_tasks:
                lines.append("")
                lines.append("    Top Tasks by CPU:")
                for task in top_tasks:
                    t_name = task.get("name", "?")
                    t_cpu = task.get("cpu_percent", 0.0)
                    t_hwm = task.get("stack_hwm", 0)
                    lines.append(
                        f"      {t_name + ':':13s} {t_cpu:.2f}%"
                        f"  (stack HWM: {t_hwm:,})"
                    )

            # NINA Instances
            nina_status = metrics.get("nina_status", {})
            instances = nina_status.get("instances", [])
            if instances:
                lines.append("")
                lines.append("    NINA Instances:")
                for i, inst in enumerate(instances):
                    state = inst.get("connection_state", "unknown")
                    ws = inst.get("websocket_connected", False)
                    ws_str = f" (ws: {'yes' if ws else 'no'})"
                    lines.append(
                        f"      Instance {i}: {state}{ws_str}"
                    )

            lines.append("")

        lines.append("=" * 72)
        lines.append(f"  END OF REPORT -- {verdict}")
        lines.append("=" * 72)

        report = "\n".join(lines)

        # Write to file
        if output_path is None:
            output_path = f"logs/report_{now.strftime('%Y%m%d_%H%M%S')}.txt"
        with open(output_path, "w") as f:
            f.write(report)
        logger.info("Report written to %s", output_path)

        return report

    def write_influx_summary(self, influx_writer):
        """Write a test_run summary measurement to InfluxDB."""
        violations = self.alert_monitor.get_violations()
        critical_count = self.alert_monitor.get_critical_count()
        passed = not self.alert_monitor.has_critical_violations()

        high_count = sum(1 for v in violations if v.severity.value == "HIGH")
        medium_count = sum(1 for v in violations if v.severity.value == "MEDIUM")

        influx_writer.write(
            "test_run",
            tags={},
            fields={
                "passed": 1 if passed else 0,
                "total_violations": len(violations),
                "critical_violations": critical_count,
                "high_violations": high_count,
                "medium_violations": medium_count,
                "cycles": self.phase_manager.cycle_count,
                "duration_s": self.phase_manager.get_total_elapsed_s(),
                "device_count": len(self.metrics_collector.devices),
            },
        )

    @staticmethod
    def _fmt_pct(value) -> str:
        """Format a percentage value with 1 decimal, or 'N/A'."""
        if value is None:
            return "N/A"
        return f"{value:.1f}%"

    @staticmethod
    def _format_duration(seconds: float) -> str:
        """Format seconds into a human-readable duration."""
        s = int(seconds)
        days = s // 86400
        hours = (s % 86400) // 3600
        minutes = (s % 3600) // 60
        secs = s % 60
        parts = []
        if days:
            parts.append(f"{days}d")
        if hours:
            parts.append(f"{hours}h")
        if minutes:
            parts.append(f"{minutes}m")
        parts.append(f"{secs}s")
        return " ".join(parts)
