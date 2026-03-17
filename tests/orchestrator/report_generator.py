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

            if metrics:
                lines.append(f"    Last heap_free:  {metrics.get('heap_free', 'N/A')}")
                lines.append(f"    Last psram_free: {metrics.get('psram_free', 'N/A')}")
                lines.append(f"    Last uptime_s:   {metrics.get('uptime_s', 'N/A')}")
                lines.append(f"    Last core0_load: {metrics.get('core0_load', 'N/A')}")
                lines.append(f"    Last core1_load: {metrics.get('core1_load', 'N/A')}")
                lines.append(f"    WiFi connected:  {metrics.get('wifi_connected', 'N/A')}")
                lines.append(f"    RSSI:            {metrics.get('rssi', 'N/A')}")
            else:
                lines.append("    No metrics available")
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
