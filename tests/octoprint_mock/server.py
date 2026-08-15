#!/usr/bin/env python3
"""
Mock OctoPrint server for ESP32-P4 NINA Display device testing.

Serves the endpoints the firmware's OctoPrint page polls, backed by fixtures
captured from a live OctoPrint 1.11.8 instance, driven by a simulated print
timeline or by manual sliders in the browser control panel at /mock/.

    python tests\\octoprint_mock\\server.py --port 5001
    python tests\\octoprint_mock\\server.py --self-test

Python 3 standard library only. No pip dependencies.
"""
import argparse
import json
import os
import random
import re
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote, quote

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")

DEFAULT_KEY = "MOCKAPIKEY000000000000000000000A"

# Byte-exact reproduction of the live 403 (fixtures/auth_behavior.json:
# content-length 140, which is Flask's pretty-printed jsonify plus a newline).
FORBIDDEN_BODY = (
    '{\n  "error": "You don\'t have the permission to access the requested '
    'resource. It is either read-protected or not readable by the server."\n}\n'
).encode()
assert len(FORBIDDEN_BODY) == 140, len(FORBIDDEN_BODY)

# Timeline shape. HEATUP then PRINT (--duration) then FINISH, then loop.
HEATUP_S = 45.0
FINISH_S = 30.0
TOOL_TARGET = 250.0
BED_TARGET = 85.0
AMBIENT_TOOL = 24.0
AMBIENT_BED = 23.0
CHAMBER = 46.07
LAYER_TOTAL = 86
LAYER_HEIGHT = 0.2
FILE_SIZE_JOB = 2647867          # /api/job reports this
FILE_SIZE_META = 2650998         # /api/files reports a different one (captured quirk)
JOB_FILE = "creative-pebble-v2-15degree-tilt-stand_0.4n_0.2mm_PETG_MINIIS_1h28m.gcode"

APPKEY_EXPIRY_S = 300.0

SCENARIOS = ("printing", "operational", "paused", "error",
             "printer_closed", "octoprint_down_60s")


def load_fixture(name):
    with open(os.path.join(FIXTURES, name), "rb") as f:
        data = f.read()
    return json.loads(data) if name.endswith(".json") else data


def hms(seconds):
    """DisplayLayerProgress-style '1h16m' / '16m' string."""
    seconds = max(0, int(seconds))
    h, m = seconds // 3600, (seconds % 3600) // 60
    return "%dh%02dm" % (h, m) if h else "%dm" % m


class MockState:
    """Timeline plus manual overrides. All access under the lock."""

    def __init__(self, duration, api_keys):
        self.lock = threading.RLock()
        self.duration = float(duration)
        self.api_keys = set(api_keys)
        self.scenario = "printing"
        self.dlp_enabled = True
        self.manual = False
        self.down_until = 0.0
        self.t0 = time.time()
        self.layer_total = LAYER_TOTAL
        self.pending = {}          # app_token -> {app, user, decision, created}
        # Manual overrides, seeded from the timeline the moment manual engages.
        self.m = {"progress": 0.0, "layer": 1,
                  "nozzle_actual": AMBIENT_TOOL, "nozzle_target": TOOL_TARGET,
                  "bed_actual": AMBIENT_BED, "bed_target": BED_TARGET}

    # ---- timeline ------------------------------------------------------
    def cycle_len(self):
        return HEATUP_S + self.duration + FINISH_S

    def sample(self):
        """Return (phase, progress_pct, tool_actual, tool_target, bed_actual, bed_target)."""
        if self.manual:
            m = self.m
            return ("manual", m["progress"], m["nozzle_actual"], m["nozzle_target"],
                    m["bed_actual"], m["bed_target"])

        t = (time.time() - self.t0) % self.cycle_len()
        if t < HEATUP_S:
            f = t / HEATUP_S
            return ("heatup", 0.0,
                    AMBIENT_TOOL + f * (TOOL_TARGET - AMBIENT_TOOL), TOOL_TARGET,
                    AMBIENT_BED + f * (BED_TARGET - AMBIENT_BED), BED_TARGET)
        t -= HEATUP_S
        if t < self.duration:
            return ("print", 100.0 * t / self.duration,
                    TOOL_TARGET + random.uniform(-0.5, 0.5), TOOL_TARGET,
                    BED_TARGET + random.uniform(-0.5, 0.5), BED_TARGET)
        return ("finish", 100.0, AMBIENT_TOOL, 0.0, AMBIENT_BED, 0.0)

    def enter_manual(self):
        """Freeze the timeline, seeding the sliders from where it stands."""
        if not self.manual:
            phase, p, ta, tt, ba, bt = self.sample()
            self.m.update(progress=p, layer=self.layer_of(p), nozzle_actual=ta,
                          nozzle_target=tt, bed_actual=ba, bed_target=bt)
            self.manual = True

    def layer_of(self, pct):
        n = self.layer_total
        return max(1, min(n, int(round(pct / 100.0 * n)) or 1))

    def pct_of_layer(self):
        return max(0.0, min(100.0, 100.0 * self.m["layer"] / self.layer_total))

    def snapshot(self):
        """Everything the endpoints need, derived consistently from one sample."""
        phase, pct, ta, tt, ba, bt = self.sample()
        pct = max(0.0, min(100.0, pct))
        scen = self.scenario
        printing = scen == "printing" and phase in ("heatup", "print", "manual")
        if scen == "paused":
            printing = False
        print_time = self.duration * pct / 100.0
        left = max(0.0, self.duration - print_time)
        layer = self.m["layer"] if self.manual else self.layer_of(pct)
        return {
            "phase": phase, "scenario": scen, "pct": pct,
            "printing": printing,
            "print_time": int(print_time), "left": int(left),
            "layer": layer,
            "tool_actual": round(ta, 2), "tool_target": round(tt, 2),
            "bed_actual": round(ba, 2), "bed_target": round(bt, 2),
        }


# ------------------------------------------------------------------ payloads

def job_payload(st, s):
    scen = s["scenario"]
    file_block = {
        "date": 1786715501, "display": JOB_FILE, "name": JOB_FILE,
        "origin": "local", "path": JOB_FILE, "size": FILE_SIZE_JOB,
    }
    if scen == "operational":
        return {"job": {"averagePrintTime": None, "estimatedPrintTime": None,
                        "filament": None, "file": file_block,
                        "lastPrintTime": None, "user": "_api"},
                "progress": {"completion": None, "filepos": None, "printTime": None,
                             "printTimeLeft": None, "printTimeLeftOrigin": None},
                "state": "Operational"}
    state = {"printing": "Printing", "paused": "Paused", "error": "Error",
             "printer_closed": "Offline"}.get(scen, "Printing")
    if scen == "printing" and s["phase"] == "finish":
        state = "Operational"
    return {
        "job": {"averagePrintTime": None, "estimatedPrintTime": None,
                "filament": None, "file": file_block,
                "lastPrintTime": None, "user": "_api"},
        "progress": {
            "completion": s["pct"],                              # float percent 0-100
            "filepos": int(FILE_SIZE_JOB * s["pct"] / 100.0),
            "printTime": s["print_time"],
            "printTimeLeft": s["left"],
            "printTimeLeftOrigin": "estimate",
        },
        "state": state,
    }


def printer_payload(st, s):
    scen = s["scenario"]
    flags = {"cancelling": False, "closedOrError": False, "error": False,
             "finishing": False, "operational": True, "paused": False,
             "pausing": False, "printing": False, "ready": False,
             "resuming": False, "sdReady": False}
    text, err = "Printing", ""
    if scen == "operational":
        flags.update(ready=True)
        text = "Operational"
    elif scen == "paused":
        flags.update(paused=True)
        text = "Paused"
    elif scen == "error":
        flags.update(error=True, closedOrError=True, operational=False)
        text, err = "Error", "MAXTEMP triggered on tool0"
    else:
        flags.update(printing=s["printing"])
        if not s["printing"]:
            flags.update(ready=True)
            text = "Operational"
    return {
        "sd": {"ready": False},
        "state": {"error": err, "flags": flags, "text": text},
        "temperature": {
            "A": {"actual": round(CHAMBER + random.uniform(-0.5, 0.5), 2),
                  "offset": 0, "target": 0.0},
            "bed": {"actual": s["bed_actual"], "offset": 0, "target": s["bed_target"]},
            "tool0": {"actual": s["tool_actual"], "offset": 0, "target": s["tool_target"]},
        },
    }


def connection_payload(st, s):
    base = load_fixture("connection.json")
    state = {"printing": "Printing", "operational": "Operational",
             "paused": "Paused", "error": "Error",
             "printer_closed": "Closed"}.get(s["scenario"], "Operational")
    base["current"]["state"] = state
    if s["scenario"] == "printer_closed":
        base["current"]["port"] = None
        base["current"]["baudrate"] = None
    return base


def dlp_payload(st, s):
    """DisplayLayerProgress. Numeric values are STRINGS; absent values are '-'."""
    pct = s["pct"]
    end = time.localtime(time.time() + s["left"])
    # Captured quirk: the three progress numbers do not agree (job 11.08,
    # m73 12, dlp progress 19). Keep them near each other but distinct.
    dlp_progress = min(100, int(round(pct * 1.7)))
    m73 = min(100, int(round(pct)) + 1)
    printer_state = {"printing": "printing", "operational": "operational",
                     "paused": "paused", "error": "error",
                     "printer_closed": "closed"}.get(s["scenario"], "printing")
    return {
        "currentFilename": JOB_FILE,
        "fanSpeed": "30%",
        "feedrate": "6632.618",
        "feedrateG0": "-",
        "feedrateG1": "6632.618",
        "height": {"current": "%.2f" % (s["layer"] * LAYER_HEIGHT),
                   "currentFormatted": "%.1f" % (s["layer"] * LAYER_HEIGHT),
                   "total": "-", "totalFormatted": "-"},
        "layer": {"averageLayerDuration": "-", "averageLayerDurationInSeconds": "-",
                  "current": str(s["layer"]), "lastLayerDuration": "0h:02m:49s",
                  "lastLayerDurationInSeconds": 169, "total": str(st.layer_total)},
        "print": {"changeFilamentCount": 0, "changeFilamentTimeLeft": "-",
                  "changeFilamentTimeLeftInSeconds": 0,
                  "estimatedChangedFilamentTime": "-",
                  "estimatedEndTime": time.strftime("%H:%M", end),
                  "m73progress": str(m73),
                  "printerState": printer_state,
                  "progress": str(dlp_progress),
                  "timeLeft": hms(s["left"]),
                  "timeLeftInSeconds": s["left"]},
    }


def files_payload(path):
    """File metadata with the requested name substituted throughout."""
    meta = load_fixture("files_current.json")
    name = path.rsplit("/", 1)[-1] or JOB_FILE
    stem = name[:-6] if name.lower().endswith(".gcode") else name
    meta.update(display=name, name=name, path=path, size=FILE_SIZE_META)
    meta["refs"] = {
        "download": "/downloads/files/local/" + path,
        "resource": "/api/files/local/" + path,
    }
    # Root-relative and percent-encoded, the way OctoPrint hands it to a client.
    meta["thumbnail"] = ("plugin/prusaslicerthumbnails/thumbnail/%s.png?%d"
                         % (quote(stem), 1786715502))
    return meta


# ------------------------------------------------------------------ handler

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockOctoPrint/1.11.8"
    state = None          # set on the server instance at startup
    quiet = False

    def log_message(self, fmt, *args):
        if not Handler.quiet:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    # ---- helpers -------------------------------------------------------
    def _octo_headers(self):
        self.send_header("x-clacks-overhead", "GNU Terry Pratchett")
        self.send_header("x-content-type-options", "nosniff")
        self.send_header("x-robots-tag", "noindex, nofollow, noimageindex")

    def send_bytes(self, status, ctype, body, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self._octo_headers()
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, obj, status=200, extra=None):
        self.send_bytes(status, "application/json",
                        json.dumps(obj, indent=2).encode() + b"\n", extra)

    def send_forbidden(self):
        self.send_response(403)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(FORBIDDEN_BODY)))
        self.send_header("vary", "Cookie")
        self._octo_headers()
        self.end_headers()
        self.wfile.write(FORBIDDEN_BODY)

    def authed(self):
        key = self.headers.get("X-Api-Key")
        with Handler.state.lock:
            return key is not None and key in Handler.state.api_keys

    def read_json(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n) or b"{}")
        except (ValueError, TypeError):
            return {}

    def down(self):
        """octoprint_down_60s: drop the connection with no response at all."""
        with Handler.state.lock:
            if time.time() >= Handler.state.down_until:
                return False
        try:
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        return True

    # ---- routing -------------------------------------------------------
    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_HEAD(self):
        self.route("GET")

    def route(self, method):
        st = Handler.state
        path = urlparse(self.path).path
        query = parse_qs(urlparse(self.path).query)

        # /mock/* is the control plane and stays alive even while "down".
        if path.startswith("/mock"):
            return self.mock_route(method, path)
        if self.down():
            return

        if path.startswith("/plugin/appkeys"):
            return self.appkeys_route(method, path)

        if path == "/webcam/" and query.get("action") == ["snapshot"]:
            # mjpg-streamer serves this with no authentication.
            return self.send_bytes(200, "image/jpeg", load_fixture("webcam.jpg"),
                                   {"access-control-allow-origin": "*",
                                    "cache-control": "no-store, no-cache, must-revalidate",
                                    "x-timestamp": "%.6f" % time.monotonic()})

        thumb = re.match(r"^/plugin/prusaslicerthumbnails/thumbnail/(.+)\.png$", path)
        if thumb:
            if not self.authed():
                return self.send_forbidden()
            return self.send_bytes(200, "image/png", load_fixture("thumbnail.png"),
                                   {"etag": '"1786715502"', "accept-ranges": "bytes"})

        if path == "/plugin/DisplayLayerProgress/values":
            if not self.authed():
                return self.send_forbidden()
            with st.lock:
                if not st.dlp_enabled:
                    return self.send_json({"error": "Not found"}, 404)
                return self.send_json(dlp_payload(st, st.snapshot()))

        if path.startswith("/api/"):
            if not self.authed():
                return self.send_forbidden()
            return self.api_route(path)

        self.send_json({"error": "Not found"}, 404)

    def api_route(self, path):
        st = Handler.state
        with st.lock:
            s = st.snapshot()
            if path == "/api/version":
                return self.send_json(load_fixture("version.json"))
            if path == "/api/job":
                return self.send_json(job_payload(st, s))
            if path == "/api/printer":
                if s["scenario"] == "printer_closed":
                    return self.send_json(
                        {"error": "Printer is not operational"}, 409)
                return self.send_json(printer_payload(st, s))
            if path == "/api/connection":
                return self.send_json(connection_payload(st, s))
        m = re.match(r"^/api/files/[^/]+/(.+)$", path)
        if m:
            return self.send_json(files_payload(unquote(m.group(1))))
        self.send_json({"error": "Not found"}, 404)

    # ---- appkeys -------------------------------------------------------
    def appkeys_route(self, method, path):
        st = Handler.state
        if path == "/plugin/appkeys/probe":
            self.send_response(204)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "0")
            self.send_header("vary", "Cookie")
            self.end_headers()
            return

        if path == "/plugin/appkeys/request" and method == "POST":
            body = self.read_json()
            app = body.get("app") or body.get("app_name") or "unknown"
            user = body.get("user") or body.get("user_id")
            token = uuid.uuid4().hex
            with st.lock:
                st.pending[token] = {"app": app, "user": user,
                                     "decision": None, "created": time.time()}
            dialog = "http://%s/plugin/appkeys/auth_dialog?token=%s" % (
                self.headers.get("Host", "localhost"), token)
            return self.send_json({"app_token": token, "auth_dialog": dialog},
                                  201, {"Location": "/plugin/appkeys/request/" + token})

        if path == "/plugin/appkeys/auth_dialog":
            token = (parse_qs(urlparse(self.path).query).get("token") or [""])[0]
            return self.send_bytes(200, "text/html; charset=utf-8",
                                   auth_dialog_html(token).encode())

        if path == "/plugin/appkeys/decide" and method == "POST":
            body = self.read_json()
            token, grant = body.get("token", ""), bool(body.get("decision"))
            with st.lock:
                req = st.pending.get(token)
                if req is None:
                    return self.send_json({"error": "unknown token"}, 404)
                req["decision"] = grant
                if grant:
                    req["api_key"] = uuid.uuid4().hex.upper()
                    st.api_keys.add(req["api_key"])
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        m = re.match(r"^/plugin/appkeys/request/([0-9a-fA-F]+)$", path)
        if m and method == "GET":
            token = m.group(1)
            with st.lock:
                req = st.pending.get(token)
                if req is None:
                    return self.send_json({"error": "Not found"}, 404)
                if time.time() - req["created"] > APPKEY_EXPIRY_S:
                    del st.pending[token]
                    return self.send_json({"error": "Not found"}, 404)
                if req["decision"] is True:
                    return self.send_json({"api_key": req["api_key"]})
                if req["decision"] is False:
                    del st.pending[token]
                    return self.send_json({"error": "Not found"}, 404)
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        self.send_json({"error": "Not found"}, 404)

    # ---- mock control plane -------------------------------------------
    def mock_route(self, method, path):
        st = Handler.state
        if path in ("/mock", "/mock/"):
            with open(os.path.join(HERE, "panel.html"), "rb") as f:
                return self.send_bytes(200, "text/html; charset=utf-8", f.read())

        if path == "/mock/status":
            with st.lock:
                s = st.snapshot()
                return self.send_json({
                    "scenario": st.scenario, "dlp_enabled": st.dlp_enabled,
                    "manual": st.manual, "duration_s": st.duration,
                    "phase": s["phase"], "progress": round(s["pct"], 2),
                    "layer": s["layer"], "layer_total": st.layer_total,
                    "print_time_s": s["print_time"], "print_time_left_s": s["left"],
                    "nozzle_actual": s["tool_actual"], "nozzle_target": s["tool_target"],
                    "bed_actual": s["bed_actual"], "bed_target": s["bed_target"],
                    "api_keys": sorted(st.api_keys),
                    "down_for_s": max(0, round(st.down_until - time.time())),
                })

        if path == "/mock/scenario" and method == "POST":
            name = self.read_json().get("scenario", "")
            if name not in SCENARIOS:
                return self.send_json({"error": "unknown scenario",
                                       "valid": list(SCENARIOS)}, 400)
            with st.lock:
                if name == "octoprint_down_60s":
                    st.down_until = time.time() + 60
                else:
                    st.scenario, st.down_until = name, 0.0
            return self.send_json({"ok": True, "scenario": name})

        if path == "/mock/dlp" and method == "POST":
            with st.lock:
                st.dlp_enabled = bool(self.read_json().get("enabled", True))
                return self.send_json({"ok": True, "dlp_enabled": st.dlp_enabled})

        if path == "/mock/set" and method == "POST":
            body = self.read_json()
            with st.lock:
                if body.get("resume"):
                    st.manual = False
                    # Restart the cycle at the progress the sliders were left on.
                    st.t0 = time.time() - HEATUP_S - st.duration * st.m["progress"] / 100.0
                    return self.send_json({"ok": True, "manual": False})
                fields = {"progress": (0.0, 100.0),
                          "nozzle_actual": (0.0, 300.0), "nozzle_target": (0.0, 300.0),
                          "bed_actual": (0.0, 120.0), "bed_target": (0.0, 120.0)}
                # Layer entry. "layer" is the old name for "current_layer".
                layer_keys = ("total_layers", "current_layer", "layer")
                touched = False
                for k, (lo, hi) in fields.items():
                    if k not in body:
                        continue
                    try:
                        v = float(body[k])
                    except (TypeError, ValueError):
                        return self.send_json({"error": "%s must be a number" % k}, 400)
                    st.enter_manual()
                    st.m[k] = max(lo, min(hi, v))
                    touched = True
                # Total first, so current clamps against the new total.
                for k in layer_keys:
                    if k not in body:
                        continue
                    try:
                        v = float(body[k])
                    except (TypeError, ValueError):
                        return self.send_json({"error": "%s must be a number" % k}, 400)
                    st.enter_manual()
                    if k == "total_layers":
                        st.layer_total = int(max(1, min(10000, v)))
                        st.m["layer"] = min(st.m["layer"], st.layer_total)
                    else:
                        st.m["layer"] = int(max(0, min(st.layer_total, v)))
                    touched = True
                if not touched:
                    return self.send_json(
                        {"error": "no known field set",
                         "valid": sorted(fields) + sorted(layer_keys) + ["resume"]}, 400)
                # Last writer wins: layers set the percent, otherwise percent sets the layer.
                if any(k in body for k in layer_keys):
                    st.m["progress"] = st.pct_of_layer()
                elif "progress" in body:
                    st.m["layer"] = st.layer_of(st.m["progress"])
                return self.send_json({"ok": True, "manual": True,
                                       "layer_total": st.layer_total,
                                       "values": dict(st.m)})

        self.send_json({"error": "Not found"}, 404)


def auth_dialog_html(token):
    return """<!doctype html><meta charset=utf-8><title>Authorize application</title>
<style>body{font:16px system-ui;max-width:34em;margin:4em auto;padding:0 1em}
button{font:inherit;padding:.6em 1.4em;margin-right:.5em;cursor:pointer}
#r{margin-top:1.5em;font-weight:600}</style>
<h1>Authorize application</h1>
<p>An application is requesting an API key from this mock OctoPrint server.</p>
<p>Request token: <code>%s</code></p>
<button onclick="d(true)">Approve</button><button onclick="d(false)">Deny</button>
<p id=r></p>
<script>function d(g){fetch('/plugin/appkeys/decide',{method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({token:'%s',decision:g})}).then(r=>{
document.getElementById('r').textContent = r.ok
  ? (g?'Approved. The application may now poll for its key.':'Denied.')
  : 'Unknown or expired request token.';})}</script>""" % (token, token)


# ------------------------------------------------------------------ runtime

def auto_approver(state, delay):
    """Approve every pending request after `delay` seconds, for unattended runs."""
    while True:
        time.sleep(0.5)
        now = time.time()
        with state.lock:
            for req in state.pending.values():
                if req["decision"] is None and now - req["created"] >= delay:
                    req["decision"] = True
                    req["api_key"] = uuid.uuid4().hex.upper()
                    state.api_keys.add(req["api_key"])


def build_server(port, key, duration, auto_approve, host="0.0.0.0", quiet=False):
    Handler.state = MockState(duration, [key])
    Handler.quiet = quiet
    httpd = ThreadingHTTPServer((host, port), Handler)
    httpd.daemon_threads = True
    if auto_approve is not None:
        threading.Thread(target=auto_approver, args=(Handler.state, auto_approve),
                         daemon=True).start()
    return httpd


def self_test():
    """Start on an ephemeral port and assert the contract. Returns exit code."""
    from urllib.request import Request, urlopen
    from urllib.error import HTTPError

    key = "SELFTESTKEY"
    httpd = build_server(0, key, duration=600, auto_approve=1,
                         host="127.0.0.1", quiet=True)
    base = "http://127.0.0.1:%d" % httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    def get(path, hdr=None, data=None):
        req = Request(base + path, data=data,
                      headers=dict(hdr or {}, **({"Content-Type": "application/json"}
                                                 if data else {})))
        try:
            with urlopen(req, timeout=5) as r:
                return r.status, r.read()
        except HTTPError as e:
            return e.code, e.read()

    def post(path, obj, hdr=None):
        return get(path, hdr, json.dumps(obj).encode())

    checks = 0

    def check(cond, msg):
        nonlocal checks
        checks += 1
        assert cond, msg

    st, _ = get("/plugin/appkeys/probe")
    check(st == 204, "probe should be 204, got %d" % st)

    st, body = get("/api/job")
    check(st == 403, "missing key should be 403, got %d" % st)
    check(len(body) == 140, "403 body should be 140 bytes, got %d" % len(body))

    st, _ = get("/api/job", {"X-Api-Key": "WRONGKEY123"})
    check(st == 403, "wrong key should be 403, got %d" % st)

    st, body = get("/api/job", {"X-Api-Key": key})
    check(st == 200, "good key should be 200, got %d" % st)
    comp = json.loads(body)["progress"]["completion"]
    check(isinstance(comp, float) and 0.0 <= comp <= 100.0,
          "completion should be a float percent, got %r" % comp)

    st, body = get("/plugin/DisplayLayerProgress/values", {"X-Api-Key": key})
    check(st == 200, "DLP should be 200 while enabled, got %d" % st)
    check(isinstance(json.loads(body)["layer"]["current"], str),
          "DLP layer.current must be a string")

    post("/mock/dlp", {"enabled": False})
    st, _ = get("/plugin/DisplayLayerProgress/values", {"X-Api-Key": key})
    check(st == 404, "DLP disabled should be 404, got %d" % st)
    post("/mock/dlp", {"enabled": True})

    post("/mock/set", {"progress": 55})
    st, body = get("/api/job", {"X-Api-Key": key})
    check(json.loads(body)["progress"]["completion"] == 55.0,
          "manual progress 55 should show completion 55, got %r"
          % json.loads(body)["progress"]["completion"])

    post("/mock/set", {"total_layers": 200, "current_layer": 50})
    st, body = get("/api/job", {"X-Api-Key": key})
    check(json.loads(body)["progress"]["completion"] == 25.0,
          "layer 50 of 200 should show completion 25.0, got %r"
          % json.loads(body)["progress"]["completion"])
    st, body = get("/plugin/DisplayLayerProgress/values", {"X-Api-Key": key})
    lay = json.loads(body)["layer"]
    check(lay["current"] == "50" and lay["total"] == "200",
          "DLP should report layer 50/200 as strings, got %r" % lay)

    st, body = post("/plugin/appkeys/request", {"app": "NINA Display (test)"})
    check(st == 201, "appkeys request should be 201, got %d" % st)
    token = json.loads(body)["app_token"]
    st, _ = get("/plugin/appkeys/request/" + token)
    check(st == 202, "undecided request should be 202, got %d" % st)

    granted, deadline = None, time.time() + 10
    while time.time() < deadline:
        st, body = get("/plugin/appkeys/request/" + token)
        if st == 200:
            granted = json.loads(body)["api_key"]
            break
        time.sleep(0.5)
    check(granted, "auto-approve should have granted a key within 10 s")

    st, _ = get("/api/job", {"X-Api-Key": granted})
    check(st == 200, "granted key should authenticate, got %d" % st)

    httpd.shutdown()
    print("self-test PASS: %d assertions" % checks)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=5001)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--key", default=DEFAULT_KEY, help="accepted X-Api-Key")
    ap.add_argument("--duration", type=int, default=600,
                    help="simulated print length in seconds (default 600)")
    ap.add_argument("--auto-approve", type=float, metavar="SECONDS",
                    help="auto-approve appkeys requests after N seconds")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    httpd = build_server(args.port, args.key, args.duration,
                         args.auto_approve, args.host)
    port = httpd.server_address[1]
    print("Mock OctoPrint on http://%s:%d" % (socket.gethostname(), port))
    print("  API key      : %s" % args.key)
    print("  Control panel: http://localhost:%d/mock/" % port)
    print("  Print cycle  : %ds heatup + %ds print + %ds finish, looping"
          % (HEATUP_S, args.duration, FINISH_S))
    if args.auto_approve is not None:
        print("  Appkeys      : auto-approving after %gs" % args.auto_approve)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
