#!/usr/bin/env python3
"""Render voice-alert clips into main/audio_clips/ with the ElevenLabs API.

This is the reproducible recipe for the shipped clips. It replaces the ad-hoc
renders that previously existed only in chat transcripts.

Format: the API is asked for output_format=pcm_16000, which returns headerless
raw PCM at 16 kHz, 16-bit signed little-endian, mono. That is byte-for-byte
what audio_alert.c feeds the codec, so no ffmpeg or resampling step is needed.

Voice and settings are pinned below and must not drift: every clip in
main/audio_clips/ is concatenated with every other clip at playback time, so a
timbre or loudness mismatch is audible mid-sentence.

TERMINAL PERIOD RULE (load-bearing, not cosmetic):
  Sentence-ENDING clips carry a trailing period in the prompt text. Without it
  the model ends on a rising, question-like contour and the announcement sounds
  unfinished. Mid-sentence fragments (equipment nouns, digits, "point", "rms",
  "hfr", "instance_N") must NOT have a period, or they land with a full stop in
  the middle of a sentence.
  main/fragment_voice_clips.html's CLIPS array carries DISPLAY strings which
  omit these periods. This file is the render truth, not that array.

Usage:
  $env:ELEVENLABS_API_KEY = "..."      (PowerShell)
  python tools/gen_voice_clips.py --list
  python tools/gen_voice_clips.py sequence_finished mount_parked
  python tools/gen_voice_clips.py --all --force
  python tools/gen_voice_clips.py --selftest     (offline, no API calls)

Existing files are never overwritten without --force.
Stdlib only; no pip install required.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

VOICE_ID = "EXAVITQu4vr4xnSDxMaL"   # "Sarah - Mature, Reassuring, Confident"
MODEL_ID = "eleven_multilingual_v2"
OUTPUT_FORMAT = "pcm_16000"          # headerless raw PCM, 16 kHz s16le mono

DEFAULT_STABILITY = 0.65
DEFAULT_SIMILARITY = 0.75

SAMPLE_RATE = 16000
BYTES_PER_SEC = SAMPLE_RATE * 2

CLIPS_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), os.pardir, "main", "audio_clips")

# Clips NOT produced by this script. Never render or overwrite these.
#   chime       synthesized beep, see tools/gen_placeholder_clips.py
#   boot_jingle music, converted from an m4a
#   warning, mount, dome, switch, and
#               user-supplied recordings, replaced in commit 16db6b4
NON_API_CLIPS = {
    "chime", "boot_jingle", "warning", "mount", "dome", "switch", "and",
}

# Mid-sentence fragments: these must never carry a terminal period.
MID_SENTENCE = {
    "point", "rms", "hfr", "camera", "guider", "focuser", "filterwheel",
    "rotator", "safety", "flat", "weather",
    "instance_1", "instance_2", "instance_3",
} | {"digit_%d" % i for i in range(10)}

# name -> dict(text=..., stability=..., previous_text=..., next_text=...)
# stability/similarity_boost/previous_text/next_text are optional.
CLIPS = {
    # --- identity ---------------------------------------------------------
    "instance_1":   {"text": "NINA one",    "stability": 0.5},
    "instance_2":   {"text": "NINA two",    "stability": 0.5},
    "instance_3":   {"text": "NINA three",  "stability": 0.5},

    # --- sentence parts ---------------------------------------------------
    "point":        {"text": "point"},
    "rms":          {"text": "R M S error"},      # spaced: letter-by-letter
    "hfr":          {"text": "H F R"},            # spaced: letter-by-letter
    "above_limit":  {"text": "above limit."},
    "rms_above":    {"text": "R M S is above set threshold.",
                     "previous_text": "NINA one."},
    "hfr_above":    {"text": "HFR is above set threshold.",
                     "previous_text": "NINA one."},   # not spaced, unlike "hfr"
    "unsafe":       {"text": "Unsafe conditions.", "stability": 0.8,
                     "previous_text":
                         "Warning. The observatory safety monitor has tripped."},
    "connected":    {"text": "connected."},
    "disconnected": {"text": "disconnected."},

    # --- digits -----------------------------------------------------------
    "digit_0": {"text": "zero"},  "digit_1": {"text": "one"},
    "digit_2": {"text": "two"},   "digit_3": {"text": "three"},
    "digit_4": {"text": "four"},  "digit_5": {"text": "five"},
    "digit_6": {"text": "six"},   "digit_7": {"text": "seven"},
    "digit_8": {"text": "eight"}, "digit_9": {"text": "nine"},

    # --- equipment nouns --------------------------------------------------
    "camera":      {"text": "Camera",          "stability": 0.5},
    "guider":      {"text": "Guider",          "stability": 0.5},
    "focuser":     {"text": "Focuser",         "stability": 0.5},
    "filterwheel": {"text": "Filter wheel",    "stability": 0.5},
    "rotator":     {"text": "Rotator",         "stability": 0.5},
    "safety":      {"text": "Safety monitor",  "stability": 0.5},
    "flat":        {"text": "Flat panel",      "stability": 0.5},
    "weather":     {"text": "Weather station", "stability": 0.5},

    # --- generic per-category event clips (fallback when no phrase fits) ---
    "sequence_event":  {"text": "Sequence event."},
    "focuser_event":   {"text": "Focuser event."},
    "mount_event":     {"text": "Mount event."},
    "meridian_flip":   {"text": "Meridian flip complete."},
    "guider_event":    {"text": "Guider event."},
    "safety_event":    {"text": "Safety event."},
    "error_event":     {"text": "Error reported."},
    "profile_changed": {"text": "Profile changed."},
    "dome_event":      {"text": "Dome event."},
    "flat_event":      {"text": "Flat panel event."},

    # --- per-event critical phrases ---------------------------------------
    "sequence_started":       {"text": "Sequence started."},
    "sequence_finished":      {"text": "Sequence finished."},
    "sequence_step_failed":   {"text": "Sequence step failed."},
    "autofocus_complete":     {"text": "Autofocus complete."},
    "autofocus_failed":       {"text": "Autofocus failed."},
    "meridian_flip_starting": {"text": "Meridian flip starting."},
    "mount_parked":           {"text": "Mount parked."},
    # NOT embedded: VOICE_EV_CONDITIONS_UNSAFE is a reserved enum slot with no
    # clip and no producer, because the breach engine already announces the
    # unsafe edge with the older "unsafe" clip. Kept here as the restore path;
    # rendering it writes a .pcm that no CLIP_LIST entry references, so also
    # re-add it to CLIP_LIST and EMBED_FILES if you revive the event.
    "conditions_unsafe":      {"text": "Conditions unsafe."},
    "conditions_safe":        {"text": "Conditions safe."},
    "guiding_stopped":        {"text": "Guiding stopped."},
    "dome_shutter_closed":    {"text": "Dome shutter closed."},
    "dome_shutter_opened":    {"text": "Dome shutter opened."},
    "platesolve_failed":      {"text": "Plate solve failed."},
    "camera_timeout":         {"text": "Camera download timeout."},
}


def render(name, spec, api_key):
    """Return raw PCM bytes for one clip."""
    body = {
        "text": spec["text"],
        "model_id": MODEL_ID,
        "voice_settings": {
            "stability": spec.get("stability", DEFAULT_STABILITY),
            "similarity_boost": spec.get("similarity_boost", DEFAULT_SIMILARITY),
        },
    }
    # Prosody context: shapes the contour without being spoken.
    for key in ("previous_text", "next_text"):
        if key in spec:
            body[key] = spec[key]

    url = ("https://api.elevenlabs.io/v1/text-to-speech/%s?output_format=%s"
           % (VOICE_ID, OUTPUT_FORMAT))
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"xi-api-key": api_key, "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def main():
    ap = argparse.ArgumentParser(
        description="Render voice clips into main/audio_clips/.")
    ap.add_argument("names", nargs="*", help="clip basenames to render")
    ap.add_argument("--all", action="store_true", help="render every clip")
    ap.add_argument("--force", action="store_true", help="overwrite existing files")
    ap.add_argument("--list", action="store_true", help="print the clip table and exit")
    args = ap.parse_args()

    if args.list:
        for name in sorted(CLIPS):
            print("%-24s %r" % (name, CLIPS[name]["text"]))
        print("")
        print("%d renderable clips; %d non-API clips: %s"
              % (len(CLIPS), len(NON_API_CLIPS), ", ".join(sorted(NON_API_CLIPS))))
        return 0

    names = sorted(CLIPS) if args.all else args.names
    if not names:
        ap.error("name a clip, or pass --all (see --list)")

    unknown = [n for n in names if n not in CLIPS]
    if unknown:
        blocked = [n for n in unknown if n in NON_API_CLIPS]
        if blocked:
            print("refusing: not API-rendered, see NON_API_CLIPS: %s"
                  % ", ".join(blocked), file=sys.stderr)
        other = [n for n in unknown if n not in NON_API_CLIPS]
        if other:
            print("unknown clip(s): %s" % ", ".join(other), file=sys.stderr)
        return 2

    api_key = os.environ.get("ELEVENLABS_API_KEY")
    if not api_key:
        print("ELEVENLABS_API_KEY is not set", file=sys.stderr)
        return 2

    total = 0
    for name in names:
        path = os.path.normpath(os.path.join(CLIPS_DIR, name + ".pcm"))
        if os.path.exists(path) and not args.force:
            print("%-24s skip (exists; --force to replace)" % name)
            continue
        try:
            pcm = render(name, CLIPS[name], api_key)
        except urllib.error.HTTPError as exc:
            print("%-24s HTTP %s: %s" % (name, exc.code, exc.read()[:200]),
                  file=sys.stderr)
            return 1
        if not pcm:
            print("%-24s empty response" % name, file=sys.stderr)
            return 1
        if len(pcm) % 2:
            # A trailing half-sample desyncs every later clip in the sentence;
            # audio_alert.c reads strictly in 16-bit frames.
            print("%-24s odd byte count %d, refusing to write" % (name, len(pcm)),
                  file=sys.stderr)
            return 1
        with open(path, "wb") as fh:
            fh.write(pcm)
        total += len(pcm)
        print("%-24s %7d B  %5.3f s  %r"
              % (name, len(pcm), len(pcm) / float(BYTES_PER_SEC),
                 CLIPS[name]["text"]))

    if total:
        print("---")
        print("wrote %d bytes (%.2f s)" % (total, total / float(BYTES_PER_SEC)))
    return 0


def selftest():
    """Offline checks that fail if the clip table breaks its own rules."""
    for name in MID_SENTENCE:
        assert name in CLIPS, "%s listed mid-sentence but absent from CLIPS" % name
        assert not CLIPS[name]["text"].endswith("."), \
            "%s is mid-sentence and must not end with a period" % name
    for name, spec in CLIPS.items():
        if name in MID_SENTENCE:
            continue
        assert spec["text"].endswith("."), \
            "%s ends a sentence and needs a terminal period" % name
    assert not (set(CLIPS) & NON_API_CLIPS), \
        "a NON_API clip leaked into the renderable table"
    for name, spec in CLIPS.items():
        stability = spec.get("stability", DEFAULT_STABILITY)
        assert 0.0 <= stability <= 1.0, "%s stability out of range" % name
    print("selftest ok: %d renderable clips, %d mid-sentence, %d non-API"
          % (len(CLIPS), len(MID_SENTENCE), len(NON_API_CLIPS)))


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
        sys.exit(0)
    sys.exit(main())
