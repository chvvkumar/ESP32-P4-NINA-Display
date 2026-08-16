# OctoPrint live capture fixtures

All files captured 2026-08-14 between 14:07 and 14:10 UTC from the live instance at `http://octopi.lan`,
OctoPrint 1.11.8, API 0.1, printer profile `prusa_mini` (Prusa Mini+) on `/dev/ttyACM0` at 115200 baud.

Printer state at capture: PRINTING. Job
`creative-pebble-v2-15degree-tilt-stand_0.4n_0.2mm_PETG_MINIIS_1h28m.gcode`, 11.08 percent complete,
filepos 293324 of 2647867, printTime 1047 s, printTimeLeft 4560 s (estimate origin). Layer 4 of 86,
current height 1.00 mm, tool0 252.5 C actual against 250.0 C target, bed 84.92 C actual against 85.0 C
target, chamber sensor `A` 46.07 C, fan 30 percent. SD not ready.

Capture method: `curl` GET only. No POST, PUT, PATCH, or DELETE was issued against the instance.
`/plugin/appkeys/request` was deliberately not touched; its protocol is documented in
`../appkeys_protocol.md` from the upstream specification instead.

| File | Source | Notes |
|------|--------|-------|
| `version.json` | GET /api/version | Server version strings. Smallest useful reachability probe. |
| `job.json` | GET /api/job | Job and progress while printing. `state` is `Printing`. `averagePrintTime`, `estimatedPrintTime`, `filament`, and `lastPrintTime` are all null for this first print of the file. |
| `printer.json` | GET /api/printer | State flags plus the three temperature sensors `tool0`, `bed`, and `A`. Flags show `printing: true`, `operational: true`, `ready: false`. |
| `connection.json` | GET /api/connection | Current serial connection plus the options list (baudrates, ports, printer profiles). |
| `dlp_values.json` | GET /plugin/DisplayLayerProgress/values | DisplayLayerProgress plugin. Note that several fields are strings, not numbers, including `layer.current`, `layer.total`, and `print.progress`. Unavailable values are the literal string `-`. `print.progress` (19) and `m73progress` (12) both differ from `/api/job` completion (11.08). |
| `files_current.json` | GET /api/files/local/creative-pebble-v2-15degree-tilt-stand_0.4n_0.2mm_PETG_MINIIS_1h28m.gcode | Metadata for the file of the running job. Carries plugin blocks from DisplayLayerProgress and dashboard, plus the `thumbnail` relative URL and `thumbnail_src`. `size` here (2650998) differs from the `size` reported inside `job.json` (2647867). |
| `thumbnail.png` | GET /plugin/prusaslicerthumbnails/thumbnail/creative-pebble-v2-15degree-tilt-stand_0.4n_0.2mm_PETG_MINIIS_1h28m.png?20260814085142 | PrusaSlicer embedded thumbnail. Exact bytes, 27098 bytes. Requires `X-Api-Key`. The URL in `files_current.json` is relative and carries a cache-busting query string. |
| `webcam.jpg` | GET /webcam/?action=snapshot | mjpg-streamer snapshot of the in-progress print. Exact bytes, 66773 bytes. No authentication required. |
| `probe_status.txt` | GET /plugin/appkeys/probe | Status code only, one line. 204, meaning the Application Keys workflow is supported on this instance. |
| `headers.json` | curl -D on /api/job, the webcam, the thumbnail, and the appkeys probe | Response content types and every non-obvious header, with per-endpoint notes on what a mock must reproduce. |
| `auth_behavior.json` | GET /api/job with no key and with a wrong key | Both return 403 with an identical JSON body and identical content-length, so a mock cannot and should not distinguish the two cases. |

Files not captured: `files_list.json` was not needed because a job was selected and running, so
`files_current.json` came from the live job path rather than from a recursive file listing.
