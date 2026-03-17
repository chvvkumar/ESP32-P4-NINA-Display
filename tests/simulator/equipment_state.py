"""State machines for simulated NINA equipment."""
import random
import math
from dataclasses import dataclass, field


@dataclass
class CameraState:
    """Camera state: cycles IDLE → EXPOSING → DOWNLOADING → IDLE."""
    name: str = "ZWO ASI2600MM Pro"
    exposure_time_s: float = 120.0
    state: str = "Idle"
    temperature: float = -20.0
    cooler_on: bool = True
    cooler_power: float = 70.0
    gain: int = 139
    offset: int = 30
    connected: bool = True
    _elapsed: float = 0.0

    def advance(self, dt_s: float) -> list[dict]:
        events = []
        self._elapsed += dt_s
        self.temperature += random.gauss(0, 0.05)
        self.temperature = max(-25.0, min(-15.0, self.temperature))
        self.cooler_power = 70.0 + random.gauss(0, 2.0)

        if self.state == "Idle":
            pass  # Controlled externally by timeline
        elif self.state == "Exposing":
            if self._elapsed >= self.exposure_time_s:
                self.state = "Downloading"
                self._elapsed = 0.0
        elif self.state == "Downloading":
            if self._elapsed >= 2.0:
                self.state = "Idle"
                self._elapsed = 0.0
                events.append({"event": "IMAGE-SAVE"})

        return events

    def start_exposure(self):
        self.state = "Exposing"
        self._elapsed = 0.0

    def to_dict(self) -> dict:
        return {
            "Connected": self.connected,
            "Name": self.name,
            "CameraState": self.state,
            "Temperature": round(self.temperature, 1),
            "CoolerOn": self.cooler_on,
            "CoolerPower": round(self.cooler_power, 1),
            "Gain": self.gain,
            "Offset": self.offset,
            "ExposureTime": self.exposure_time_s,
            "IsExposing": self.state == "Exposing",
            "ExposureEndTime": None,
        }


@dataclass
class GuiderState:
    """Guider: generates Gaussian jitter RMS values."""
    is_guiding: bool = False
    rms_ra: float = 0.5
    rms_dec: float = 0.3
    rms_total: float = 0.58
    settle_distance: float = 0.0
    ra_mean: float = 0.5
    ra_sigma: float = 0.15
    dec_mean: float = 0.3
    dec_sigma: float = 0.1

    def advance(self, dt_s: float) -> list[dict]:
        if self.is_guiding:
            self.rms_ra = abs(random.gauss(self.ra_mean, self.ra_sigma))
            self.rms_dec = abs(random.gauss(self.dec_mean, self.dec_sigma))
            self.rms_total = math.sqrt(self.rms_ra ** 2 + self.rms_dec ** 2)
            self.settle_distance = max(0, random.gauss(0.1, 0.05))
        return []

    def to_dict(self) -> dict:
        return {
            "Connected": True,
            "IsGuiding": self.is_guiding,
            "RMSError": {
                "RA": {"Arcseconds": round(self.rms_ra, 3)},
                "Dec": {"Arcseconds": round(self.rms_dec, 3)},
                "Total": {"Arcseconds": round(self.rms_total, 3)},
            },
            "SettleDistance": round(self.settle_distance, 3),
        }


@dataclass
class FocuserState:
    """Focuser: position with temperature drift and autofocus jumps."""
    position: int = 15000
    temperature: float = -5.0
    is_moving: bool = False
    drift_per_s: float = 1.0 / 60.0
    _drift_accum: float = 0.0

    def advance(self, dt_s: float) -> list[dict]:
        self.temperature += random.gauss(0, 0.01)
        self._drift_accum += self.drift_per_s * dt_s
        if self._drift_accum >= 1.0:
            self.position += random.choice([-1, 0, 1])
            self._drift_accum = 0.0
        return []

    def autofocus_jump(self):
        self.position += random.randint(-200, 200)

    def to_dict(self) -> dict:
        return {
            "Connected": True,
            "Position": self.position,
            "Temperature": round(self.temperature, 1),
            "IsMoving": self.is_moving,
        }


@dataclass
class MountState:
    """Mount: tracking with meridian flip simulation."""
    tracking: bool = True
    side_of_pier: str = "East"
    time_to_flip_s: float = 3600.0
    ra: float = 0.0
    dec: float = 0.0
    connected: bool = True
    flip_interval_s: float = 3600.0
    flip_jitter_s: float = 300.0
    _flip_pending: bool = False

    def advance(self, dt_s: float) -> list[dict]:
        events = []
        if self.tracking:
            self.time_to_flip_s -= dt_s
            if self.time_to_flip_s <= 0 and not self._flip_pending:
                self._flip_pending = True
                self.side_of_pier = "West" if self.side_of_pier == "East" else "East"
                self.time_to_flip_s = self.flip_interval_s + random.uniform(-self.flip_jitter_s, self.flip_jitter_s)
                self._flip_pending = False
                events.append({"event": "MERIDIAN-FLIP"})
        return events

    def slew_to(self, ra: float, dec: float):
        self.ra = ra
        self.dec = dec

    def set_timing(self, flip_interval_s: float, flip_jitter_s: float):
        self.flip_interval_s = flip_interval_s
        self.flip_jitter_s = flip_jitter_s

    def to_dict(self) -> dict:
        flip_s = max(0, round(self.time_to_flip_s))
        hours = flip_s // 3600
        minutes = (flip_s % 3600) // 60
        secs = flip_s % 60
        return {
            "Connected": self.connected,
            "Tracking": self.tracking,
            "SideOfPier": self.side_of_pier,
            "TimeToMeridianFlip": round(self.time_to_flip_s),
            "TimeToMeridianFlipString": f"{hours:02d}:{minutes:02d}:{secs:02d}",
            "RightAscension": round(self.ra, 4),
            "Declination": round(self.dec, 4),
        }


@dataclass
class FilterWheelState:
    """Filter wheel: cycles through available filters."""
    available_filters: list[str] = field(default_factory=lambda: ["Ha", "OIII", "SII"])
    current_position: int = 0

    @property
    def current_filter(self) -> str:
        return self.available_filters[self.current_position]

    def change_filter(self) -> str:
        self.current_position = (self.current_position + 1) % len(self.available_filters)
        return self.current_filter

    def to_dict(self) -> dict:
        return {
            "Connected": True,
            "SelectedFilter": {
                "Name": self.current_filter,
                "Id": self.current_position,
            },
            "AvailableFilters": [
                {"Name": f, "Id": i}
                for i, f in enumerate(self.available_filters)
            ],
        }


@dataclass
class SwitchState:
    """Power switch: voltage, amps, watts with jitter."""
    voltage: float = 12.0
    amps: float = 3.5
    pwm_channels: list[dict] = field(default_factory=lambda: [
        {"Name": "Dew Heater 1", "Value": 50.0},
        {"Name": "Dew Heater 2", "Value": 60.0},
        {"Name": "Camera Cooler", "Value": 100.0},
        {"Name": "Mount USB", "Value": 100.0},
    ])

    @property
    def watts(self) -> float:
        return self.voltage * self.amps

    def advance(self, dt_s: float) -> list[dict]:
        self.voltage = 12.0 + random.gauss(0, 0.1)
        self.amps = 3.5 + random.gauss(0, 0.2)
        return []

    def to_dict(self) -> dict:
        return {
            "Connected": True,
            "ReadonlySwitches": [
                {"Name": "Input Voltage", "Description": "Volts", "Value": round(self.voltage, 2)},
                {"Name": "Total Current", "Description": "Current", "Value": round(self.amps, 2)},
                {"Name": "Total Power", "Description": "Watts", "Value": round(self.watts, 2)},
            ] + [
                {"Name": ch["Name"], "Description": "PWM power output", "Value": ch["Value"]}
                for ch in self.pwm_channels
            ],
            "WritableSwitches": [],
        }


@dataclass
class SafetyMonitorState:
    """Safety monitor: toggles unsafe for 2 min every 2 hours."""
    is_safe: bool = True
    _elapsed_s: float = 0.0
    _unsafe_duration: float = 120.0
    _safe_duration: float = 7200.0

    def advance(self, dt_s: float) -> list[dict]:
        events = []
        self._elapsed_s += dt_s

        if self.is_safe:
            if self._elapsed_s >= self._safe_duration:
                self.is_safe = False
                self._elapsed_s = 0.0
                events.append({"event": "SAFETY-CHANGED", "IsSafe": False})
        else:
            if self._elapsed_s >= self._unsafe_duration:
                self.is_safe = True
                self._elapsed_s = 0.0
                events.append({"event": "SAFETY-CHANGED", "IsSafe": True})

        return events

    def set_timing(self, safe_duration_s: float, unsafe_duration_s: float):
        self._safe_duration = safe_duration_s
        self._unsafe_duration = unsafe_duration_s

    def to_dict(self) -> dict:
        return {
            "Connected": True,
            "IsSafe": self.is_safe,
        }
