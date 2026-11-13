#!/usr/bin/env python3
"""Serve web/index.html with a fake BlueBird API, to work on the dashboard without a PS5.

The fake adapter is a TP-Link UB500. Search finds two speakers and a phone; Connect walks
through calling, pairing and opening before it plays. --no-adapter starts with nothing plugged.

Usage: python tools/mock_dashboard.py [port] [--no-adapter]   (default port 8120)
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "web" / "index.html"
START = time.time()
LOCK = threading.Lock()

ADAPTER = {"present": True, "path": "/dev/ugen1.3", "id": "2357:0604", "chip": "Realtek RTL8761B",
           "address": "E8:48:B8:C8:20:00", "firmware": 1, "clone": False, "acl_mtu": 1021, "acl_bufs": 8,
           "prefer": ""}
FOUND = [
    {"addr": "F4:4E:FD:12:34:56", "name": "JBL Flip 5", "rssi": -48, "audio": True, "cod": "240414"},
    {"addr": "00:1A:7D:DA:71:13", "name": "Caixinha Bluetooth", "rssi": -71, "audio": True, "cod": "240404"},
    {"addr": "A0:B1:C2:D3:E4:F5", "name": "Galaxy S23", "rssi": -60, "audio": False, "cod": "5a020c"},
]
S = {
    "bt": "ready", "adapter": dict(ADAPTER), "auto": True, "scanning": False, "found": [],
    "saved": [{"addr": "F4:4E:FD:12:34:56", "name": "JBL Flip 5", "paired": True}],
    "link": {"state": "idle", "code": 0, "addr": "", "name": "", "incoming": False, "error": "", "mtu": 0,
             "media_sent": 0, "media_dropped": 0},
    "volume": 80, "link_since": 0, "target": None, "scan_until": 0, "probe_at": 0,
}
STATES = [(0, "idle"), (1, "calling"), (2, "pairing"), (4, "choosing codec"), (5, "opening stream"), (7, "streaming")]
LOG = []


def log(msg):
    t = time.strftime("%H:%M:%S")
    LOG.append(f"[{t}.000] {msg}")


def tick():
    now = time.time()
    if S["scanning"] and now >= S["scan_until"]:
        S["scanning"] = False
        log("host: search done")
    if S["scanning"]:
        n = int((now - (S["scan_until"] - 12)) / 2)
        S["found"] = FOUND[:max(0, min(n, len(FOUND)))]
    t = S["target"]
    if t:
        step = min(int((now - S["link_since"]) / 1.0) + 1, len(STATES) - 1)
        code, name = STATES[step]
        if S["link"]["code"] != code:
            log(f"host: -> {name}")
        S["link"].update({"state": name, "code": code, "addr": t["addr"], "name": t["name"], "mtu": 895 if code >= 5 else 0})
        if code == 7:
            S["link"]["media_sent"] += 53
            if not any(p["addr"] == t["addr"] for p in S["saved"]):
                S["saved"].insert(0, {"addr": t["addr"], "name": t["name"], "paired": True})


def probe():
    if not S["probe_at"]:
        return {"state": 0, "sources": []}
    if time.time() - S["probe_at"] < 4:
        return {"state": 1, "sources": []}
    rows = []
    for src in range(8):
        opened = src in (0, 1, 2, 3)
        rows.append({"source": src, "opened": opened, "result": "opened" if opened else "OpenAudio failed 0x81950001",
                     "bytes_per_s": [384000, 0, 384000, 96000][src] if opened else 0,
                     "level": [41, 0, 38, 22][src] if opened else 0, "nonzero": 90, "format": "float32"})
    return {"state": 2, "sources": rows}


def state():
    with LOCK:
        tick()
        streaming = S["link"]["code"] == 7
        sent = S["link"]["media_sent"]
        return {
            "version": "0.1.0", "build": 1, "lang": "pt", "port": 8120, "uptime": int(time.time() - START),
            "bt": S["bt"], "adapter": S["adapter"],
            "adapters": [{"path": "/dev/ugen1.3", "id": "2357:0604", "in_use": True}] if S["adapter"]["present"] else [],
            "link": S["link"],
            "stream": {"active": streaming, "rate": 48000 if streaming else 0, "bitpool": 53 if streaming else 0,
                       "bitpool_max": 53 if streaming else 0, "frames_per_packet": 7, "packet_ms": 18.7,
                       "sent": sent, "dropped": 0, "late": 0, "underruns": 1, "skips": 0, "buffered_ms": 52,
                       "volume": S["volume"], "capture": "capturing (rights 0)" if streaming else "not started",
                       "capturing": streaming},
            "probe": probe(), "auto": S["auto"], "scanning": S["scanning"], "found": S["found"], "saved": S["saved"],
        }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif u.path == "/start":
            self.send(200, (ROOT / "web" / "start.html").read_bytes(), "text/html; charset=utf-8")
        elif u.path == "/bluebird.appcache":
            self.send(200, "CACHE MANIFEST\n# mock\n\nCACHE:\n/start\n\nNETWORK:\n*\n", "text/cache-manifest")
        elif u.path == "/ping":
            self.send(200, "bluebird-ok", "text/plain")
        elif u.path == "/ping.gif":
            self.send(200, bytes.fromhex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b"), "image/gif")
        elif u.path == "/api/state":
            self.send(200, json.dumps(state()))
        elif u.path == "/api/log":
            self.send(200, "\n".join(LOG[-300:]) + "\n", "text/plain; charset=utf-8")
        else:
            self.send(404, "not found", "text/plain")

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        ok = True
        with LOCK:
            if u.path == "/api/scan":
                S["scanning"], S["scan_until"], S["found"] = True, time.time() + 12, []
                log("host: searching for speakers for 12.8 s")
            elif u.path == "/api/connect":
                pool = FOUND + S["saved"]
                t = next((x for x in pool if x["addr"] == q.get("addr")), None)
                ok = t is not None
                if ok:
                    S["target"], S["link_since"] = t, time.time()
                    S["link"]["media_sent"] = 0
            elif u.path == "/api/disconnect":
                S["target"] = None
                S["link"].update({"state": "idle", "code": 0, "addr": "", "name": ""})
            elif u.path == "/api/forget":
                S["saved"] = [p for p in S["saved"] if p["addr"] != q.get("addr")]
            elif u.path == "/api/volume":
                S["volume"] = int(q.get("v", 100))
            elif u.path == "/api/auto":
                S["auto"] = q.get("on") == "1"
            elif u.path == "/api/probe":
                S["probe_at"] = time.time()
                log("capture: probe of 8 sources")
            elif u.path in ("/api/tone", "/api/adapter"):
                pass
            else:
                ok = False
        self.send(200 if ok else 409, json.dumps({"ok": ok}))


def main():
    port = 8120
    for a in sys.argv[1:]:
        if a == "--no-adapter":
            S["adapter"]["present"] = False
            S["bt"] = "looking for an adapter"
        elif a.isdigit():
            port = int(a)
    log("main: mock BlueBird started")
    print(f"mock dashboard on http://localhost:{port}/")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
