#!/usr/bin/env python3
"""Generate placeholder voice-alert clips into main/audio_clips/.

Each clip is a short beep pattern (distinct tone + pulse count per clip) so an
assembled sentence is audibly distinguishable before the real ElevenLabs
renders land. Output format matches what audio_alert.c opens the codec with:
16 kHz, 16-bit signed little-endian, mono, headerless raw PCM.

Run from anywhere: python tools/gen_placeholder_clips.py
Overwrites existing .pcm files. Replace individual files with real renders in
the same format; no code or CMake change is needed.
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
    "unsafe":      (330,  4, 70,  30),
    "point":       (1500, 1, 150, 0),
}
# digit_N: pitch rises with the digit; one pulse for 0-4, two for 5-9 as a
# coarse group cue. A pulse-per-digit scheme would push digit_9 past 400 ms.
for _d in range(10):
    _count = 1 + _d // 5
    CLIPS["digit_%d" % _d] = (400 + 60 * _d, _count, 160 if _count == 1 else 150, 40)


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

    for name, params in sorted(CLIPS.items()):
        data = render(*params)
        with open(os.path.join(out_dir, name + ".pcm"), "wb") as f:
            f.write(data)
        print("%-14s %6d bytes  %4d ms" %
              (name + ".pcm", len(data), len(data) * 1000 // (2 * SAMPLE_RATE)))

    print("%d clips written to %s" % (len(CLIPS), out_dir))
    return 0


def _self_check():
    """Sanity: every clip is non-empty, even-sized (whole s16 samples), and in
    the 150-400 ms range the sentence timing assumes."""
    for name, params in CLIPS.items():
        data = render(*params)
        ms = len(data) * 1000 // (2 * SAMPLE_RATE)
        assert len(data) and len(data) % 2 == 0, name
        assert 150 <= ms <= 400, "%s is %d ms" % (name, ms)
    assert len(CLIPS) == 20, len(CLIPS)
    print("self-check OK")


if __name__ == "__main__":
    if "--self-check" in sys.argv:
        _self_check()
    else:
        sys.exit(main())
