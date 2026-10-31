#!/usr/bin/env python3
"""Dock display designer: a local web page with a mock of the device.

    python3 tools/designer/serve.py          # then open http://localhost:8250
    python3 tools/designer/serve.py 8300     # another port

- Draws each frame with the firmware's own ui.h and render.cpp, compiled on
  your computer (c++ from the Xcode command line tools is enough). Edit
  render.cpp and the page picks it up on the next redraw: the renderer is
  rebuilt whenever a source file changes.
- Fetches live TfL counts and Santander battery levels for the mock, the
  same calls the device makes. A web page can't make the battery call
  itself, so this server makes both.
- Reads and writes docks.json, and writes docks_config.h alongside it.

Needs Elecrow's driver files first: ./tools/fetch_driver.sh
Standard library only. Listens on 127.0.0.1 only.
"""
import http.server
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
SKETCH = os.path.join(ROOT, "firmware", "dock_display")
BUILD = os.path.join(HERE, "build")
CLI = os.path.join(BUILD, "render_cli")
STATIC = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/style.css": "style.css"}
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8"}

TFL_LIST = "https://api.tfl.gov.uk/BikePoint"
GQL_URL = "https://santandercycles.tfl.gov.uk/bikesharefe-gql"
# Same query as santander.cpp.
SUPPLY_QUERY = (
    "query GetSystemSupply($input: SupplyInput) { supply(input: $input) { stations { "
    "stationId bikesAvailable bikeDocksAvailable ebikesAvailable totalBikesAvailable isOffline "
    "ebikes { batteryStatus { percent distanceRemaining { value unit } } } } } }"
)
USER_AGENT = "london-dock-display-designer/0.2"
LIVE_CACHE_S = 30

spec = importlib.util.spec_from_file_location("docks_json", os.path.join(TOOLS, "docks_json.py"))
docks_json = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docks_json)


# ------------------------------------------------------------------ renderer
SOURCES = [
    os.path.join(HERE, "render_cli.cpp"),
    *(os.path.join(SKETCH, f) for f in ("render.cpp", "render.h", "gfx.cpp", "gfx.h", "docks.h", "ui.h", "EPD.cpp", "EPD.h", "EPDfont.h")),
]
_build_lock = threading.Lock()


def ensure_renderer():
    """(Re)builds render_cli if a source is newer. Returns an error string or None."""
    missing = [p for p in SOURCES if not os.path.exists(p)]
    if any(p.endswith(("EPD.cpp", "EPD.h", "EPDfont.h")) for p in missing):
        return "Elecrow's driver files are missing: run ./tools/fetch_driver.sh first"
    if missing:
        return "missing " + ", ".join(os.path.relpath(p, ROOT) for p in missing)
    with _build_lock:
        newest = max(os.path.getmtime(p) for p in SOURCES)
        if os.path.exists(CLI) and os.path.getmtime(CLI) >= newest:
            return None
        os.makedirs(BUILD, exist_ok=True)
        cxx = os.environ.get("CXX", "c++")
        cmd = [cxx, "-std=c++17", "-O1", "-I", os.path.join(TOOLS, "preview"), "-I", SKETCH, "-o", CLI,
               os.path.join(HERE, "render_cli.cpp"), *(os.path.join(SKETCH, f) for f in ("render.cpp", "gfx.cpp", "EPD.cpp"))]
        print("building renderer:", " ".join(os.path.relpath(c, ROOT) if c.startswith(ROOT) else c for c in cmd))
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except FileNotFoundError:
            return f"no C++ compiler ({cxx}); install the Xcode command line tools"
        if out.returncode != 0:
            return "renderer failed to build:\n" + (out.stderr or out.stdout)[-4000:]
    return None


def _clean(s, limit):
    return "".join(c for c in str(s) if " " <= c <= "~")[:limit]


def frame(req):
    """Turns a /api/frame request into render_cli input, runs it, returns its answer."""
    err = ensure_renderer()
    if err:
        return {"error": err}
    ui = req.get("ui", {})
    ctx = req.get("ctx", {})
    lines = [f"ui {int(ui.get('screen', 0))} {int(ui.get('nScreens', 1))} {1 if ui.get('status') else 0}"]
    if req.get("key") in ("up", "down", "ok", "back", "menu"):
        lines.append(f"key {req['key']}")
    lines.append(f"ctx {int(ctx.get('dataMin', -1))} {1 if ctx.get('stale') else 0} {1 if ctx.get('busy') else 0}")
    for i, s in enumerate(req.get("slots", [])[:8]):
        if not s:
            continue
        lines.append("slot %d %d %d %d %d %d %d %d %d" % (
            i, int(s.get("id", 0)), 1 if s.get("hasCounts") else 0, 1 if s.get("closed") else 0,
            int(s.get("bikes", 0)), int(s.get("ebikes", 0)), int(s.get("spaces", 0)), int(s.get("docks", -1)),
            1 if s.get("hasBatt") else 0))
        batt = [str(max(0, min(100, int(p)))) for p in s.get("batt", [])[:255]]
        if batt:
            lines.append(f"batt {i} " + " ".join(batt))
        lines.append(f"label {i} {_clean(s.get('label', ''), 16)}")
    if req.get("status"):
        lines.append("status " + "|".join(_clean(x, 23).replace("|", "/") for x in req["status"][:5]))
    if req.get("message"):
        lines.append("message " + "|".join(_clean(x, 30).replace("|", "/") for x in req["message"][:2]))
    out = subprocess.run([CLI], input="\n".join(lines) + "\n", capture_output=True, text=True, timeout=10)
    if out.returncode != 0:
        return {"error": "renderer crashed: " + out.stderr[-2000:]}
    return json.loads(out.stdout)


# ------------------------------------------------------------------ live data
_cache = {}


def _cached(key, fetch):
    hit = _cache.get(key)
    if hit and time.time() - hit[0] < LIVE_CACHE_S:
        return hit[1]
    value = fetch()
    _cache[key] = (time.time(), value)
    return value


def _http_json(url, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={
        "User-Agent": USER_AGENT, "Accept": "application/json",
        **({"Content-Type": "application/json"} if data else {}),
    })
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def bikepoint_counts(place):
    """Same reading of TfL's fields as tflBikePoint() in santander.cpp."""
    props = {p.get("key"): p.get("value") for p in place.get("additionalProperties", [])}

    def num(k, default=-1):
        try:
            return int(props.get(k))
        except (TypeError, ValueError):
            return default

    nb, std, e = num("NbBikes"), num("NbStandardBikes"), num("NbEBikes", 0)
    return {
        "id": int(str(place.get("id", "")).rsplit("_", 1)[-1] or 0),
        "name": place.get("commonName", ""),
        "lat": place.get("lat"), "lon": place.get("lon"),
        "hasCounts": nb >= 0 and num("NbEmptyDocks") >= 0,
        "closed": str(props.get("Locked", "")).lower() == "true" or str(props.get("Installed", "true")).lower() != "true",
        "bikes": std if std >= 0 else max(0, nb - e),
        "ebikes": e,
        "spaces": num("NbEmptyDocks", 0),
        "docks": num("NbDocks"),
    }


def bikepoints():
    places = _cached("tfl", lambda: _http_json(TFL_LIST))
    docks = [bikepoint_counts(p) for p in places]
    return sorted((d for d in docks if d["id"] > 0), key=lambda d: d["name"].lower())


def batteries(ids):
    """{id: [percent, ...]} for the asked-for docks; ids missing from the feed are left out."""
    answer = _cached("gql", lambda: _http_json(GQL_URL, {
        "query": SUPPLY_QUERY, "variables": {"input": {"regionCode": "london"}}}))
    if answer.get("errors"):
        raise RuntimeError("; ".join(e.get("message", "?") for e in answer["errors"]))
    stations = ((answer.get("data") or {}).get("supply") or {}).get("stations") or []
    out = {}
    for st in stations:
        try:
            sid = int(str(st.get("stationId")).rsplit("_", 1)[-1])
        except ValueError:
            continue
        if sid in ids:
            out[str(sid)] = [int((e.get("batteryStatus") or {}).get("percent") or 0) for e in st.get("ebikes") or []]
    return {"batteries": out, "stations": len(stations)}


# ------------------------------------------------------------------ server
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        if not self.path.startswith("/api/frame"):
            sys.stderr.write("%s\n" % (fmt % args))

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in STATIC:
            name = STATIC[path]
            with open(os.path.join(HERE, name), "rb") as f:
                return self._send(200, f.read(), TYPES[os.path.splitext(name)[1]])
        try:
            if path == "/api/sources":
                # The page polls this and redraws when render.cpp etc. change.
                return self._send(200, {"mtime": max((os.path.getmtime(p) for p in SOURCES if os.path.exists(p)), default=0)})
            if path == "/api/config":
                with open(docks_json.JSON_PATH) as f:
                    return self._send(200, {"config": json.load(f)})
            if path == "/api/bikepoints":
                return self._send(200, {"docks": bikepoints()})
            if path == "/api/batteries":
                query = self.path.split("?", 1)[1] if "?" in self.path else ""
                params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
                ids = {int(x) for x in params.get("ids", "").split(",") if x.isdigit()}
                return self._send(200, batteries(ids))
        except (urllib.error.URLError, TimeoutError, OSError, RuntimeError, ValueError) as e:
            return self._send(502, {"error": f"{type(e).__name__}: {e}"})
        self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/api/frame":
            return self._send(404, {"error": "not found"})
        try:
            self._send(200, frame(self._body()))
        except (ValueError, TypeError, subprocess.TimeoutExpired) as e:
            self._send(400, {"error": f"{type(e).__name__}: {e}"})

    def do_PUT(self):
        if self.path != "/api/config":
            return self._send(404, {"error": "not found"})
        try:
            cfg = self._body()
        except ValueError as e:
            return self._send(400, {"errors": [f"not JSON: {e}"]})
        errors = docks_json.write(cfg)
        if errors:
            return self._send(400, {"errors": errors})
        self._send(200, {"ok": True, "header": docks_json.header(cfg),
                         "paths": [os.path.relpath(docks_json.JSON_PATH, ROOT), os.path.relpath(docks_json.HEADER_PATH, ROOT)]})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8250
    err = ensure_renderer()
    if err:
        print("warning:", err, file=sys.stderr)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"dock display designer on http://localhost:{port}  (Ctrl-C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
