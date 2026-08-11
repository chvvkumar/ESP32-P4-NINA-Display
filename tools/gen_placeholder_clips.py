#!/usr/bin/env python3
"""Generate placeholder voice-alert clips into main/audio_clips/.

Each clip is a short beep pattern (distinct tone + pulse count per clip) so an
assembled sentence is audibly distinguishable before the real ElevenLabs
renders land. Output format matches what audio_alert.c opens the codec with:
16 kHz, 16-bit signed little-endian, mono, headerless raw PCM.

Run from anywhere: python tools/gen_placeholder_clips.py
Skips any .pcm that already exists (real renders must never be clobbered by
beeps; this has happened once). Pass --force to overwrite existing files.
Replace individual files with real renders in the same format; no code or
CMake change is needed.
"""

import math
import os
import struct
import sys

SAMPLE_RATE = 16000
AMPLITUDE = 0.35          # headroom: sentences are concatenated, not mixed
FADE_MS = 5               # de-click ramp on each pulse edge

# name -> (tone Hz, pulse count, pulse ms, gap ms). Total length stays in the
# 150-400 ms band the plan calls for.
CLIPS = {
    "chime":       (1320, 2, 90,  30),
    "warning":     (440,  3, 90,  40),
    "instance_1":  (700,  1, 200, 0),
    "instance_2":  (700,  2, 90,  30),
    "instance_3":  (700,  3, 70,  30),
    "rms":         (520,  2, 120, 40),
    "hfr":         (620,  2, 120, 40),
    "above_limit": (880,  3, 100, 30),
    "rms_above":   (980, 2, 130, 40),
    "hfr_above":   (1020, 2, 130, 40),
    "unsafe":      (330,  4, 70,  30),
    "point":       (1500, 1, 150, 0),
}
# digit_N: pitch rises with the digit; one pulse for 0-4, two for 5-9 as a
# coarse group cue. A pulse-per-digit scheme would push digit_9 past 400 ms.
for _d in range(10):
    _count = 1 + _d // 5
    CLIPS["digit_%d" % _d] = (400 + 60 * _d, _count, 160 if _count == 1 else 150, 40)

# Event-announcement clips (audio_alert_speak_event). Equipment names match
# equipment_names[] in nina_websocket.c, snake_cased: one long pulse, pitch
# identifies the device. Category event clips: two pulses, pitch identifies
# the category. connected/disconnected: high vs low double-beep.
_EQUIPMENT = ["camera", "mount", "guider", "focuser", "filterwheel", "rotator",
              "safety", "dome", "flat", "switch", "weather"]
for _i, _name in enumerate(_EQUIPMENT):
    CLIPS[_name] = (480 + 45 * _i, 1, 180, 0)

_EVENTS = ["sequence_event", "focuser_event", "mount_event", "meridian_flip",
           "guider_event", "safety_event", "error_event", "profile_changed",
           "dome_event", "flat_event"]
for _i, _name in enumerate(_EVENTS):
    CLIPS[_name] = (450 + 55 * _i, 2, 110, 40)

CLIPS["connected"]    = (1180, 2, 90, 30)
CLIPS["disconnected"] = (340,  2, 90, 30)
CLIPS["and"]          = (1050, 1, 160, 0)   # list separator in grouped sentences

# Real renders that live in main/audio_clips/ and must NEVER be overwritten by
# placeholders (boot_jingle.pcm is a real 5 s render, not a beep pattern).
REAL_RENDERS = {"boot_jingle"}


def pulse(freq_hz, ms):
    """One sine burst with linear fade in/out, as a list of float samples."""
    n = int(SAMPLE_RATE * ms / 1000)
    fade = max(1, int(SAMPLE_RATE * FADE_MS / 1000))
    out = []
    for i in range(n):
        env = min(1.0, i / fade, (n - i) / fade)
        out.append(AMPLITUDE * env * math.sin(2 * math.pi * freq_hz * i / SAMPLE_RATE))
    return out


def render(freq_hz, count, pulse_ms, gap_ms):
    silence = [0.0] * int(SAMPLE_RATE * gap_ms / 1000)
    samples = []
    for i in range(count):
        samples += pulse(freq_hz, pulse_ms)
        if i != count - 1:
            samples += silence
    return struct.pack("<%dh" % len(samples),
                       *(int(max(-1.0, min(1.0, s)) * 32767) for s in samples))


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "main", "audio_clips")
    out_dir = os.path.normpath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    force = "--force" in sys.argv
    written = skipped = 0
    for name, params in sorted(CLIPS.items()):
        if name in REAL_RENDERS:
            continue
        path = os.path.join(out_dir, name + ".pcm")
        if os.path.exists(path) and not force:
            skipped += 1
            continue
        data = render(*params)
        with open(path, "wb") as f:
            f.write(data)
        written += 1
        print("%-14s %6d bytes  %4d ms" %
              (name + ".pcm", len(data), len(data) * 1000 // (2 * SAMPLE_RATE)))

    print("%d written, %d skipped (existing; --force overwrites) in %s" %
          (written, skipped, out_dir))
    return 0


def _self_check():
    """Sanity: every clip is non-empty, even-sized (whole s16 samples), and in
    the 150-400 ms range the sentence timing assumes."""
    for name, params in CLIPS.items():
        data = render(*params)
        ms = len(data) * 1000 // (2 * SAMPLE_RATE)
        assert len(data) and len(data) % 2 == 0, name
        assert 150 <= ms <= 400, "%s is %d ms" % (name, ms)
    assert len(CLIPS) == 46, len(CLIPS)
    print("self-check OK")


if __name__ == "__main__":
    if "--self-check" in sys.argv:
        _self_check()
    else:
        sys.exit(main())
