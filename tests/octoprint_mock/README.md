# Mock OctoPrint server

Serves the endpoints the firmware's OctoPrint page polls, backed by fixtures captured from a live
OctoPrint 1.11.8 instance (see `fixtures/README.md`). Point a physical ESP32-P4 display at it to
exercise the page without running a real print.

Python 3 standard library only. No pip install step, no dependency on the `tests/` harness.

## Start

```
python tests\octoprint_mock\server.py --port 5001
```

Startup prints the accepted API key and the control panel URL. The server binds `0.0.0.0` so the
display can reach it over the network.

| Flag | Default | Purpose |
|------|---------|---------|
| `--port N` | 5001 | Listen port. |
| `--host ADDR` | 0.0.0.0 | Bind address. |
| `--key K` | `MOCKAPIKEY000000000000000000000A` | Accepted `X-Api-Key`. |
| `--duration N` | 600 | Simulated print length in seconds. |
| `--auto-approve N` | off | Grant every Application Keys request N seconds after it arrives. |
| `--self-test` | | Start on an ephemeral port, assert the contract, exit. |

Windows Firewall prompts on first run. Allow it on private networks or the display cannot connect.

## Point the display at it

1. Open the display web UI at `http://ninadashN.lan/config` and select the OctoPrint tab.
2. Set the URL to `http://desktop-bdt978m.lan:5001` (this PC). Use the hostname, not an IP.
3. Set the API key to the value the server printed at startup, or run the Application Keys flow
   (see below) with `--auto-approve 3`.
4. Enable the OctoPrint page and save.

## Simulated print

The default mode loops one cycle: 45 s heatup (temperatures ramp to target, state Printing,
completion 0), then the print over `--duration` seconds (completion 0 to 100, printTime and
printTimeLeft consistent, layer 1 to 86, DLP `estimatedEndTime` at wall clock plus remaining,
temperatures jittering plus or minus 0.5 C), then 30 s finished (Operational, completion 100).

Captured quirks are preserved:

- DisplayLayerProgress numeric values are JSON strings, and absent values are the literal `-`.
- The three progress numbers do not agree: `/api/job` completion, DLP `print.progress`, and DLP
  `print.m73progress` each track the print at their own offset, as on the real instance.
- `/api/job` completion is a float percent 0 to 100.
- `/api/files` reports a different `size` than `/api/job` for the same file.
- Missing and wrong API keys both return the identical 403 body, byte for byte, 140 bytes.

## Endpoints

| Path | Auth | Notes |
|------|------|-------|
| `GET /api/version` | key | Fixture verbatim. |
| `GET /api/job` | key | Timeline or manual values. |
| `GET /api/printer` | key | 409 in the `printer_closed` scenario. |
| `GET /api/connection` | key | `current.state` follows the scenario. |
| `GET /api/files/local/<path>` | key | Any path returns fixture metadata with the name substituted. |
| `GET /plugin/DisplayLayerProgress/values` | key | 404 when the plugin is toggled off. |
| `GET /plugin/prusaslicerthumbnails/thumbnail/<name>.png` | key | Serves `fixtures/thumbnail.png`. |
| `GET /webcam/?action=snapshot` | none | Serves `fixtures/webcam.jpg` as `image/jpeg`. |
| `GET /plugin/appkeys/probe` | none | 204. |
| `POST /plugin/appkeys/request` | none | 201 with `Location`, `app_token`, `auth_dialog`. |
| `GET /plugin/appkeys/request/<token>` | none | 202 pending, 200 granted, 404 denied or expired. |
| `GET /plugin/appkeys/auth_dialog?token=<t>` | none | Approve and Deny page. |
| `GET /mock/` | none | Browser control panel. |
| `GET /mock/status` | none | Current scenario, mode, and timeline position. |
| `POST /mock/scenario` | none | Switch scenario. |
| `POST /mock/dlp` | none | Enable or disable the DisplayLayerProgress endpoint. |
| `POST /mock/set` | none | Set values by hand, or resume the timeline. |

## Control panel

Open `http://localhost:5001/mock/` in a browser. It carries sliders for progress, layer, nozzle
actual and target, and bed actual and target, plus the scenario buttons and the DisplayLayerProgress
toggle. Moving any slider switches the mock into manual mode: the timeline pauses and the values are
held as set. Dependent values stay consistent, so printTime, printTimeLeft, file position, ETA, and
the M73 percentage all follow the progress slider, and DisplayLayerProgress values remain strings.

Select Resume timeline to return to the simulated print. It restarts the print phase at the progress
the sliders were left on.

## Scenario control by script

```
curl -X POST -d "{\"scenario\": \"printing\"}"        http://localhost:5001/mock/scenario
curl -X POST -d "{\"scenario\": \"operational\"}"     http://localhost:5001/mock/scenario
curl -X POST -d "{\"scenario\": \"paused\"}"          http://localhost:5001/mock/scenario
curl -X POST -d "{\"scenario\": \"error\"}"           http://localhost:5001/mock/scenario
curl -X POST -d "{\"scenario\": \"printer_closed\"}"  http://localhost:5001/mock/scenario
curl -X POST -d "{\"scenario\": \"octoprint_down_60s\"}" http://localhost:5001/mock/scenario

curl -X POST -d "{\"enabled\": false}" http://localhost:5001/mock/dlp
curl -X POST -d "{\"enabled\": true}"  http://localhost:5001/mock/dlp

curl http://localhost:5001/mock/status
```

| Scenario | Effect |
|----------|--------|
| `printing` | Default. The timeline drives job, printer, and DLP. |
| `operational` | Idle. `/api/job` progress fields are null, state Operational, printer ready. |
| `paused` | Job state Paused, printer flag `paused` true, progress frozen. |
| `error` | Printer `state.flags.error` true and `state.error` carries text. |
| `printer_closed` | `/api/connection` `current.state` is `Closed` and `/api/printer` returns 409. |
| `octoprint_down_60s` | Every endpoint except `/mock/*` drops the connection for 60 seconds. The previous scenario resumes afterwards. |

Disabling DisplayLayerProgress makes its endpoint return 404, which exercises the firmware's 404
latch: the client stops asking until the next reconnect.

## Setting values by script

`POST /mock/set` takes any of `progress` (0 to 100), `layer` (0 to 86), `nozzle_actual`,
`nozzle_target` (0 to 300), `bed_actual`, `bed_target` (0 to 120). Any of them switches the mock into
manual mode. Out of range values are clamped. Setting `progress` without `layer` derives the layer
from the progress.

```
curl -X POST -d "{\"progress\": 55}"                    http://localhost:5001/mock/set
curl -X POST -d "{\"progress\": 99.5, \"layer\": 85}"   http://localhost:5001/mock/set
curl -X POST -d "{\"nozzle_actual\": 215, \"nozzle_target\": 250}" http://localhost:5001/mock/set
curl -X POST -d "{\"bed_actual\": 60, \"bed_target\": 85}" http://localhost:5001/mock/set
curl -X POST -d "{\"resume\": true}"                    http://localhost:5001/mock/set
```

PowerShell quotes single quotes differently. Use `curl.exe` with the escaping above, or:

```
Invoke-RestMethod -Method Post -Uri http://localhost:5001/mock/set -Body '{"progress": 55}'
```

## Application Keys walkthrough

The mock implements the workflow documented in `appkeys_protocol.md`. Keys it grants become accepted
`X-Api-Key` values for the rest of the run.

Attended, from the display:

1. Start the mock without `--auto-approve`.
2. In the display web UI OctoPrint tab, set the URL and select the button that requests access.
3. The firmware probes, posts the request, and polls. Open `http://localhost:5001/mock/` and read
   the request token from the status box, or open the `auth_dialog` URL the firmware reports.
4. The dialog page has Approve and Deny buttons. Approve grants a generated key, which the firmware
   saves. Deny makes the poll return 404 and the firmware report a decline.

Unattended: start with `--auto-approve 3` and every request is granted three seconds after it
arrives, with no browser step.

By hand with curl:

```
curl -i http://localhost:5001/plugin/appkeys/probe
curl -i -X POST -d "{\"app\": \"test\"}" http://localhost:5001/plugin/appkeys/request
curl -i http://localhost:5001/plugin/appkeys/request/<app_token>
curl -X POST -d "{\"token\": \"<app_token>\", \"decision\": true}" http://localhost:5001/plugin/appkeys/decide
curl -i http://localhost:5001/plugin/appkeys/request/<app_token>
```

Both field spellings are accepted on the request, `app` and `user` as well as `app_name` and
`user_id`.

Deviations from the real plugin, deliberate:

- Requests expire after 5 minutes rather than after 5 seconds without a poll. The real plugin drops a
  request that is not polled for 5 seconds, which makes manual curl testing impractical.
- The decision endpoint is `POST /plugin/appkeys/decide` with the token in the body, not
  `POST /plugin/appkeys/decision/<user_token>`. There is no user session in the mock, so there is no
  user token to issue. Nothing the firmware calls is affected: the client only ever touches `probe`,
  `request`, and `request/<app_token>`.

## Self-test

```
python tests\octoprint_mock\server.py --self-test
```

Starts on an ephemeral port and asserts: probe returns 204, a missing key returns the 140 byte 403,
a wrong key returns 403, a good key returns 200 with a float completion, DLP returns strings and
returns 404 once disabled, `POST /mock/set` progress 55 shows completion 55 on `/api/job`, and the
Application Keys flow with `--auto-approve` grants a key that then passes authentication.

## Layout

| File | Purpose |
|------|---------|
| `server.py` | The mock. Single file, standard library only. |
| `panel.html` | Browser control panel served at `/mock/`. |
| `fixtures/` | Live capture from `octopi.lan`, with its own README. |
| `appkeys_protocol.md` | Application Keys workflow specification. |
