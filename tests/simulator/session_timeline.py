"""Session timeline that drives a simulated imaging night."""
import random
import time
from dataclasses import dataclass, field

from .equipment_state import (
    CameraState, GuiderState, FocuserState, MountState,
    FilterWheelState, SwitchState, SafetyMonitorState,
)

TARGETS = [
    ("M31 - Andromeda Galaxy", 0.7122, 41.2689),
    ("M42 - Orion Nebula", 5.5881, -5.3911),
    ("NGC 2244 - Rosette Nebula", 6.5317, 4.9528),
    ("IC 1396 - Elephant Trunk", 21.6278, 57.4969),
    ("NGC 7000 - North America Nebula", 20.9789, 44.3178),
    ("M81 - Bode's Galaxy", 9.9256, 69.0653),
    ("NGC 6992 - Eastern Veil", 20.8233, 31.7178),
    ("M51 - Whirlpool Galaxy", 13.4983, 47.1953),
]

INSTANCE_CONFIGS = [
    {
        "profile_name": "Deep Sky Rig",
        "telescope": "APM 140/1050",
        "camera_name": "ZWO ASI2600MM Pro",
        "exposure_time": 120,
        "filters": ["Ha", "OIII", "SII"],
    },
    {
        "profile_name": "Widefield Rig",
        "telescope": "Samyang 135mm",
        "camera_name": "ZWO ASI533MC Pro",
        "exposure_time": 60,
        "filters": ["L", "R", "G", "B"],
    },
    {
        "profile_name": "Planetary Rig",
        "telescope": "C11 EdgeHD",
        "camera_name": "ZWO ASI294MM Pro",
        "exposure_time": 30,
        "filters": ["Ha", "L"],
    },
]

SPEED_PROFILES = {
    "fast": {
        "exposure_times": [15, 15, 15],
        "slew_duration_s": 1,
        "autofocus_point_interval_s": 0.5,
        "autofocus_settle_s": 0.5,
        "guiding_settle_s": 1,
        "dither_duration_s": 1,
        "filter_change_duration_s": 0.5,
        "target_complete_duration_s": 0.5,
        "exposures_per_filter_range": [2, 3],
        "target_change_interval": 6,
        "guider_ra_mean": 0.8,
        "guider_ra_sigma": 0.3,
        "guider_dec_mean": 0.5,
        "guider_dec_sigma": 0.2,
        "focuser_drift_per_s": 5.0 / 60.0,
        "mount_flip_interval_s": 300,
        "mount_flip_jitter_s": 60,
        "safety_safe_duration_s": 120,
        "safety_unsafe_duration_s": 30,
    },
    "normal": {
        "exposure_times": [120, 60, 30],
        "slew_duration_s": 2,
        "autofocus_point_interval_s": 1.0,
        "autofocus_settle_s": 1.0,
        "guiding_settle_s": 2,
        "dither_duration_s": 2,
        "filter_change_duration_s": 1,
        "target_complete_duration_s": 1,
        "exposures_per_filter_range": [3, 6],
        "target_change_interval": 10,
        "guider_ra_mean": 0.5,
        "guider_ra_sigma": 0.15,
        "guider_dec_mean": 0.3,
        "guider_dec_sigma": 0.1,
        "focuser_drift_per_s": 1.0 / 60.0,
        "mount_flip_interval_s": 3600,
        "mount_flip_jitter_s": 300,
        "safety_safe_duration_s": 7200,
        "safety_unsafe_duration_s": 120,
    },
}


class SessionPhase:
    CONNECTING = "connecting"
    SLEWING = "slewing"
    AUTOFOCUS = "autofocus"
    GUIDING_START = "guiding_start"
    EXPOSING = "exposing"
    DITHERING = "dithering"
    FILTER_CHANGE = "filter_change"
    TARGET_COMPLETE = "target_complete"


class SessionTimeline:
    """Drives a realistic astrophotography session for one simulator instance."""

    def __init__(self, instance_index: int, speed_profile: str = "normal"):
        cfg = INSTANCE_CONFIGS[instance_index]
        self._profile = SPEED_PROFILES[speed_profile]
        self.instance_index = instance_index
        self.profile_name = cfg["profile_name"]
        self.telescope = cfg["telescope"]
        self.exposure_time = self._profile["exposure_times"][instance_index]

        self.camera = CameraState(
            name=cfg["camera_name"],
            exposure_time_s=self._profile["exposure_times"][instance_index],
        )
        self.guider = GuiderState(
            is_guiding=True,
            ra_mean=self._profile["guider_ra_mean"],
            ra_sigma=self._profile["guider_ra_sigma"],
            dec_mean=self._profile["guider_dec_mean"],
            dec_sigma=self._profile["guider_dec_sigma"],
        )
        self.focuser = FocuserState(drift_per_s=self._profile["focuser_drift_per_s"])
        self.mount = MountState(
            tracking=True,
            time_to_flip_s=random.uniform(1800, 5400),
            ra=random.uniform(0, 24),
            dec=random.uniform(-30, 70),
            flip_interval_s=self._profile["mount_flip_interval_s"],
            flip_jitter_s=self._profile["mount_flip_jitter_s"],
        )
        self.filter_wheel = FilterWheelState(available_filters=list(cfg["filters"]))
        self.switch = SwitchState()
        self.safety = SafetyMonitorState(
            _safe_duration=self._profile["safety_safe_duration_s"],
            _unsafe_duration=self._profile["safety_unsafe_duration_s"],
        )

        # Start in CONNECTING but with fast transitions
        self.phase = SessionPhase.CONNECTING
        self._phase_elapsed: float = 0.0
        self._target_index: int = random.randint(0, len(TARGETS) - 1)
        self._current_target: str = TARGETS[self._target_index][0]
        self._exposures_this_filter: int = 0
        epf = self._profile["exposures_per_filter_range"]
        self._exposures_per_filter: int = random.randint(epf[0], epf[1])
        self._total_exposures: int = 0
        self._image_history: list[dict] = []
        self._af_points_sent: int = 0
        self._sequence_started: bool = False

        # Stats
        self.requests_served: int = 0
        self.ws_connections: int = 0
        self.events_emitted: int = 0

    def advance(self, dt_s: float) -> list[dict]:
        """Advance all equipment states and the session timeline."""
        events = []
        self._phase_elapsed += dt_s

        # Equipment state updates — guider and camera always update for live data
        cam_events = self.camera.advance(dt_s)
        self.guider.advance(dt_s)
        self.focuser.advance(dt_s)
        mount_events = self.mount.advance(dt_s)
        self.switch.advance(dt_s)
        safety_events = self.safety.advance(dt_s)

        events.extend(safety_events)
        events.extend(mount_events)

        # Phase state machine — fast transitions to get to exposing quickly
        if self.phase == SessionPhase.CONNECTING:
            if self._phase_elapsed >= 1.0:  # 1s connect (was 3s)
                events.append({"event": "CAMERA-CONNECTED", "Name": self.camera.name})
                events.append({"event": "MOUNT-CONNECTED", "Name": self.telescope})
                self.camera.connected = True
                self.mount.connected = True
                self._transition(SessionPhase.SLEWING)

        elif self.phase == SessionPhase.SLEWING:
            if self._phase_elapsed >= self._profile["slew_duration_s"]:
                target_name, ra, dec = TARGETS[self._target_index]
                self._current_target = target_name
                self.mount.slew_to(ra, dec)
                events.append({
                    "event": "TS-NEWTARGETSTART",
                    "Name": target_name,
                    "RA": ra,
                    "Dec": dec,
                })
                if not self._sequence_started:
                    events.append({"event": "SEQUENCE-STARTING"})
                    self._sequence_started = True
                self._transition(SessionPhase.AUTOFOCUS)

        elif self.phase == SessionPhase.AUTOFOCUS:
            if self._phase_elapsed < 1.0:
                if self._af_points_sent == 0:
                    events.append({"event": "AUTOFOCUS-STARTING"})
            # Send V-curve points quickly (1s apart instead of 3s)
            num_points = 7
            point_interval = self._profile["autofocus_point_interval_s"]
            expected_point = int(self._phase_elapsed / point_interval)
            if expected_point > self._af_points_sent and self._af_points_sent < num_points:
                focus_pos = self.focuser.position + (self._af_points_sent - 3) * 50
                hfr = 1.5 + abs(self._af_points_sent - 3) * 0.8 + random.gauss(0, 0.1)
                events.append({
                    "event": "AUTOFOCUS-POINT-ADDED",
                    "Position": focus_pos,
                    "HFR": round(hfr, 2),
                })
                self._af_points_sent += 1

            if self._phase_elapsed >= num_points * point_interval + self._profile["autofocus_settle_s"]:
                self.focuser.autofocus_jump()
                events.append({
                    "event": "AUTOFOCUS-FINISHED",
                    "Position": self.focuser.position,
                })
                self._af_points_sent = 0
                self._transition(SessionPhase.GUIDING_START)

        elif self.phase == SessionPhase.GUIDING_START:
            if self._phase_elapsed >= self._profile["guiding_settle_s"]:
                self.guider.is_guiding = True
                events.append({"event": "GUIDER-START"})
                self._transition(SessionPhase.EXPOSING)
                self.camera.start_exposure()

        elif self.phase == SessionPhase.EXPOSING:
            for ev in cam_events:
                if ev["event"] == "IMAGE-SAVE":
                    self._total_exposures += 1
                    self._exposures_this_filter += 1
                    hfr = random.uniform(1.5, 3.5)
                    stars = random.randint(80, 400)
                    img_data = {
                        "event": "IMAGE-SAVE",
                        "HFR": round(hfr, 2),
                        "Stars": stars,
                        "Filter": self.filter_wheel.current_filter,
                        "Temperature": round(self.camera.temperature, 1),
                        "Gain": self.camera.gain,
                        "ExposureTime": self.exposure_time,
                        "Target": self._current_target,
                    }
                    events.append(img_data)
                    self._image_history.append({
                        "HFR": round(hfr, 2),
                        "Stars": stars,
                        "Filter": self.filter_wheel.current_filter,
                        "Target": self._current_target,
                        "Timestamp": time.time(),
                    })
                    # Start next exposure or transition
                    if self._exposures_this_filter >= self._exposures_per_filter:
                        self._transition(SessionPhase.DITHERING)
                    else:
                        self._transition(SessionPhase.DITHERING)

        elif self.phase == SessionPhase.DITHERING:
            if self._phase_elapsed >= self._profile["dither_duration_s"]:
                events.append({"event": "GUIDER-DITHER"})
                if self._exposures_this_filter >= self._exposures_per_filter:
                    self._transition(SessionPhase.FILTER_CHANGE)
                else:
                    self._transition(SessionPhase.EXPOSING)
                    self.camera.start_exposure()

        elif self.phase == SessionPhase.FILTER_CHANGE:
            if self._phase_elapsed >= self._profile["filter_change_duration_s"]:
                new_filter = self.filter_wheel.change_filter()
                self._exposures_this_filter = 0
                epf = self._profile["exposures_per_filter_range"]
                self._exposures_per_filter = random.randint(epf[0], epf[1])
                events.append({
                    "event": "FILTERWHEEL-CHANGED",
                    "Filter": new_filter,
                })
                # Move to next target every ~10 exposures
                if self._total_exposures > 0 and self._total_exposures % self._profile["target_change_interval"] == 0:
                    self._transition(SessionPhase.TARGET_COMPLETE)
                else:
                    self._transition(SessionPhase.EXPOSING)
                    self.camera.start_exposure()

        elif self.phase == SessionPhase.TARGET_COMPLETE:
            if self._phase_elapsed >= self._profile["target_complete_duration_s"]:
                events.append({"event": "SEQUENCE-FINISHED"})
                self._sequence_started = False
                self._target_index = (self._target_index + 1) % len(TARGETS)
                self._transition(SessionPhase.SLEWING)

        self.events_emitted += len(events)
        return events

    def _transition(self, new_phase: str):
        self.phase = new_phase
        self._phase_elapsed = 0.0

    def set_speed_profile(self, name: str):
        """Switch to a different speed profile mid-run."""
        self._profile = SPEED_PROFILES[name]
        self.exposure_time = self._profile["exposure_times"][self.instance_index]
        self.camera.exposure_time_s = self.exposure_time
        epf = self._profile["exposures_per_filter_range"]
        self._exposures_per_filter = random.randint(epf[0], epf[1])
        # Propagate to equipment states
        self.guider.ra_mean = self._profile["guider_ra_mean"]
        self.guider.ra_sigma = self._profile["guider_ra_sigma"]
        self.guider.dec_mean = self._profile["guider_dec_mean"]
        self.guider.dec_sigma = self._profile["guider_dec_sigma"]
        self.focuser.drift_per_s = self._profile["focuser_drift_per_s"]
        self.mount.set_timing(
            self._profile["mount_flip_interval_s"],
            self._profile["mount_flip_jitter_s"],
        )
        self.safety.set_timing(
            self._profile["safety_safe_duration_s"],
            self._profile["safety_unsafe_duration_s"],
        )

    # ── REST response builders ──

    def get_camera_info(self) -> dict:
        self.requests_served += 1
        return self.camera.to_dict()

    def get_guider_info(self) -> dict:
        self.requests_served += 1
        return self.guider.to_dict()

    def get_focuser_info(self) -> dict:
        self.requests_served += 1
        return self.focuser.to_dict()

    def get_mount_info(self) -> dict:
        self.requests_served += 1
        return self.mount.to_dict()

    def get_filter_info(self) -> dict:
        self.requests_served += 1
        return self.filter_wheel.to_dict()

    def get_switch_info(self) -> dict:
        self.requests_served += 1
        return self.switch.to_dict()

    def get_safety_info(self) -> dict:
        self.requests_served += 1
        return self.safety.to_dict()

    def get_equipment_info(self) -> dict:
        self.requests_served += 1
        return {
            "Camera": self.camera.to_dict(),
            "Guider": self.guider.to_dict(),
            "Focuser": self.focuser.to_dict(),
            "Mount": self.mount.to_dict(),
            "FilterWheel": self.filter_wheel.to_dict(),
            "Switch": self.switch.to_dict(),
            "SafetyMonitor": self.safety.to_dict(),
        }

    def get_profile(self) -> list:
        self.requests_served += 1
        return [{
            "Name": self.profile_name,
            "IsActive": True,
        }]

    def get_sequence_json(self) -> list:
        self.requests_served += 1
        filters = self.filter_wheel.available_filters
        filter_items = []
        for i, f in enumerate(filters):
            completed = max(0, self._total_exposures // len(filters) - i)
            total = self._exposures_per_filter * 3
            filter_items.append({
                "Name": f"{f}_Container",
                "Items": [{
                    "Name": f"{f} Exposure",
                    "ExposureCount": total,
                    "ExposureCountCompleted": completed,
                    "ExposureTime": self.exposure_time,
                }],
            })
        return [{
            "Name": "Targets_Container",
            "Items": [{
                "Name": f"{self._current_target}_Container",
                "Items": filter_items,
                "Status": 2 if self.phase == "exposing" else 0,
            }],
        }]

    def get_image_history(self, count_only: bool = False):
        self.requests_served += 1
        if count_only:
            return len(self._image_history)
        # Return in reverse order (newest first) with firmware-expected field names
        result = []
        for img in reversed(self._image_history[-50:]):
            result.append({
                "TargetName": img.get("Target", self._current_target),
                "TelescopeName": self.telescope,
                "ExposureTime": self.exposure_time,
                "Filter": img.get("Filter", ""),
                "HFR": img.get("HFR", 0),
                "Stars": img.get("Stars", 0),
            })
        return result

    def get_stats(self) -> dict:
        return {
            "requests_served": self.requests_served,
            "ws_connections": self.ws_connections,
            "events_emitted": self.events_emitted,
        }
