#!/usr/bin/env python3
"""Mock ESP32 server for testing the MosquiddoDeth web UI.

Usage:
  python3 mock-server.py          # Normal mode (zone control UI)
  python3 mock-server.py --setup  # Setup mode (WiFi captive portal)
"""

import json
import sys
import time
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

DATA_DIR = Path(__file__).parent / "data"
SRC_DIR = Path(__file__).parent / "src"
LOG_FILE = Path(__file__).parent / "mock-runlog.jsonl"
PORT = 8080
SETUP_MODE = "--setup" in sys.argv

# ── Fake device state ──
state = {
    "device_id": "a1b2",
    "hostname": "mosquiddo-deth-a1b2",
    "friendly_name": "",
    "pin": "",
    "active_zone": -1,
    "queue": [],
    "tz_offset": -18000,
    "tz_dst": 3600,
    "geo_lat": 30.2672,
    "geo_lon": -97.7431,
    "rain_check": True,
    "rain_paused": False,
    "rain_pause_until": 0,
    "rain_skip": False,
    "tank_enabled": True,
    "tank_empty_cm": 100,
    "tank_full_cm": 10,
    "tank_percent": 62,
    "tank_distance_cm": 44.2,
    "update_version": "1.4.0",
    "update_notes": "Simulated update for testing",
    "zones": [
        {"name": "Front Yard", "seconds": 90, "active": False, "start": 0, "pin": 26, "queued": False, "trigger": "", "schedules": [
            {"enabled": True, "days": 0b0100100, "hour": 6, "minute": 0}
        ]},
        {"name": "Back Patio", "seconds": 120, "active": False, "start": 0, "pin": 25, "queued": False, "trigger": "", "schedules": []},
        {"name": "Side Yard", "seconds": 60, "active": False, "start": 0, "pin": 33, "queued": False, "trigger": "", "schedules": []},
        {"name": "Garden", "seconds": 45, "active": False, "start": 0, "pin": 32, "queued": False, "trigger": "", "schedules": []},
    ],
}
lock = threading.Lock()


def log_run(zone_idx, trigger, seconds):
    entry = {
        "t": time.strftime("%Y-%m-%d %H:%M:%S"),
        "z": zone_idx,
        "name": state["zones"][zone_idx]["name"],
        "trigger": trigger,
        "sec": seconds,
    }
    with open(LOG_FILE, "a") as f:
        f.write(json.dumps(entry) + "\n")
    # Trim at 64KB
    if LOG_FILE.exists() and LOG_FILE.stat().st_size > 65536:
        lines = LOG_FILE.read_text().splitlines()
        LOG_FILE.write_text("\n".join(lines[len(lines)//2:]) + "\n")


def check_auto_stop():
    """Background thread to auto-stop zones and process queue."""
    while True:
        with lock:
            az = state["active_zone"]
            if az >= 0:
                z = state["zones"][az]
                if z["active"] and time.time() - z["start"] >= z["seconds"]:
                    ran = int(time.time() - z["start"])
                    log_run(az, z.get("trigger", "unknown"), ran)
                    z["active"] = False
                    state["active_zone"] = -1
                    print(f"[Zone {az}] {z['name']} auto-stopped ({ran}s)")
                    # Process queue
                    if state["queue"]:
                        nz = state["queue"].pop(0)
                        state["zones"][nz]["active"] = True
                        state["zones"][nz]["start"] = time.time()
                        state["zones"][nz]["trigger"] = "queue"
                        state["active_zone"] = nz
                        print(f"[Zone {nz}] {state['zones'][nz]['name']} started from queue")
            # Update queued flags
            for i, z in enumerate(state["zones"]):
                z["queued"] = i in state["queue"]
            # Update rain pause
            if state["rain_pause_until"] > 0 and time.time() >= state["rain_pause_until"]:
                state["rain_pause_until"] = 0
                state["rain_paused"] = False
        time.sleep(1)


def status_json():
    with lock:
        az = state["active_zone"]
        doc = {
            "device_id": state["device_id"],
            "hostname": state["hostname"],
            "friendly_name": state["friendly_name"],
            "pin_enabled": len(state["pin"]) > 0,
            "version": "1.3.0",
            "ip": "192.168.1.100",
            "uptime": int(time.time()) % 86400,
            "wifi_rssi": -52,
            "active_zone": az,
            "queue_size": len(state["queue"]),
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "weekday": int(time.strftime("%w")),
            "tz_offset": state["tz_offset"],
            "tz_dst": state["tz_dst"],
            "geo_lat": state["geo_lat"],
            "geo_lon": state["geo_lon"],
            "rain_check": state["rain_check"],
            "rain_paused": state["rain_paused"],
            "rain_skip": state["rain_skip"],
            "tank_enabled": state["tank_enabled"],
            "tank_percent": state["tank_percent"],
            "tank_distance_cm": state["tank_distance_cm"],
            "tank_empty_cm": state["tank_empty_cm"],
            "tank_full_cm": state["tank_full_cm"],
            "tank_low": state["tank_enabled"] and state["tank_percent"] <= 15,
            "update_version": state.get("update_version", ""),
            "update_notes": state.get("update_notes", ""),
            "zones": [],
        }
        for z in state["zones"]:
            remaining = 0
            if z["active"]:
                elapsed = time.time() - z["start"]
                remaining = max(0, z["seconds"] - int(elapsed))
            doc["zones"].append({
                "name": z["name"],
                "seconds": z["seconds"],
                "active": z["active"],
                "pin": z["pin"],
                "remaining": remaining,
                "queued": z["queued"],
                "schedules": z["schedules"],
            })
        return doc


def check_pin(body):
    if not state["pin"]:
        return True
    return body.get("pin", "") == state["pin"]


def extract_setup_html():
    """Extract the setup page HTML from main.cpp PROGMEM string."""
    cpp = (SRC_DIR / "main.cpp").read_text()
    start = cpp.find('R"rawliteral(') + len('R"rawliteral(')
    end = cpp.find(')rawliteral"')
    if start > 0 and end > start:
        return cpp[start:end]
    return "<html><body>Setup HTML not found</body></html>"


def extract_ota_html():
    """Extract the OTA page HTML from main.cpp."""
    cpp = (SRC_DIR / "main.cpp").read_text()
    # Find the second rawliteral block (OTA page)
    first_end = cpp.find(')rawliteral"')
    if first_end < 0:
        return "<html><body>OTA HTML not found</body></html>"
    search_from = first_end + len(')rawliteral"')
    start = cpp.find('R"rawliteral(', search_from)
    if start < 0:
        return "<html><body>OTA HTML not found</body></html>"
    start += len('R"rawliteral(')
    end = cpp.find(')rawliteral"', start)
    if end > start:
        return cpp[start:end]
    return "<html><body>OTA HTML not found</body></html>"


SETUP_HTML = extract_setup_html()
OTA_HTML = extract_ota_html()

# Fake WiFi networks for setup mode
FAKE_NETWORKS = [
    {"ssid": "HomeNetwork-5G", "rssi": -42},
    {"ssid": "HomeNetwork", "rssi": -55},
    {"ssid": "Neighbors_WiFi", "rssi": -71},
    {"ssid": "NETGEAR-Guest", "rssi": -78},
]


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if SETUP_MODE:
            return self.do_GET_setup()
        if self.path == "/":
            self.serve_file("index.html", "text/html")
        elif self.path == "/apple-touch-icon.png":
            self.serve_file("apple-touch-icon.png", "image/png")
        elif self.path == "/icon-512.png":
            self.serve_file("icon-512.png", "image/png")
        elif self.path == "/logo.png":
            self.serve_file("logo.png", "image/png")
        elif self.path == "/update":
            self.html_response(OTA_HTML)
        elif self.path == "/api/status":
            self.json_response(status_json())
        elif self.path == "/api/devices":
            self.json_response({"devices": [
                {"id": state["device_id"], "hostname": state["hostname"],
                 "name": state["friendly_name"], "ip": "192.168.1.100",
                 "active": state["active_zone"] >= 0, "self": True},
                {"id": "c3d4", "hostname": "mosquiddo-deth-c3d4",
                 "name": "Neighbor's Yard", "ip": "192.168.1.101",
                 "active": False, "self": False},
            ]})
        elif self.path == "/api/log":
            if LOG_FILE.exists():
                data = LOG_FILE.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", len(data))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", 0)
                self.end_headers()
        else:
            self.send_error(404)

    def do_GET_setup(self):
        if self.path == "/logo.png":
            self.serve_file("logo.png", "image/png")
        elif self.path == "/api/scan":
            self.json_response({"networks": FAKE_NETWORKS})
        else:
            self.html_response(SETUP_HTML)

    def do_POST(self):
        body = self.read_body()

        if SETUP_MODE and self.path == "/api/setup":
            ssid = body.get("ssid", "")
            name = body.get("name", "")
            print(f"[Setup] WiFi: '{ssid}', Name: '{name}'")
            print("[Setup] In real device, this would save creds and reboot")
            self.json_response({"ok": True, "hostname": state["hostname"]})
            return

        if self.path == "/api/zone/start":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            z = body.get("zone", -1)
            with lock:
                if 0 <= z < len(state["zones"]):
                    if state["active_zone"] >= 0:
                        if z not in state["queue"]:
                            state["queue"].append(z)
                            print(f"[Zone {z}] queued")
                    else:
                        state["zones"][z]["active"] = True
                        state["zones"][z]["start"] = time.time()
                        state["zones"][z]["trigger"] = "manual"
                        state["active_zone"] = z
                        print(f"[Zone {z}] {state['zones'][z]['name']} started")
            self.json_response(status_json())

        elif self.path == "/api/zone/stop":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            with lock:
                if body.get("all"):
                    for i, z in enumerate(state["zones"]):
                        if z["active"]:
                            ran = int(time.time() - z["start"])
                            log_run(i, z.get("trigger", "manual"), ran)
                        z["active"] = False
                    state["active_zone"] = -1
                    state["queue"].clear()
                    print("[Zones] All stopped")
                else:
                    z = body.get("zone", -1)
                    if 0 <= z < len(state["zones"]):
                        if state["zones"][z]["active"]:
                            ran = int(time.time() - state["zones"][z]["start"])
                            log_run(z, state["zones"][z].get("trigger", "manual"), ran)
                        state["zones"][z]["active"] = False
                        if state["active_zone"] == z:
                            state["active_zone"] = -1
            self.json_response(status_json())

        elif self.path == "/api/zone/run-all":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            with lock:
                for i in range(len(state["zones"])):
                    if state["active_zone"] < 0:
                        state["zones"][i]["active"] = True
                        state["zones"][i]["start"] = time.time()
                        state["zones"][i]["trigger"] = "run-all"
                        state["active_zone"] = i
                    elif i != state["active_zone"] and i not in state["queue"]:
                        state["queue"].append(i)
                print(f"[Zones] Run all — active={state['active_zone']}, queue={state['queue']}")
            self.json_response(status_json())

        elif self.path == "/api/config":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            with lock:
                if "friendly_name" in body:
                    state["friendly_name"] = body["friendly_name"]
                if "new_pin" in body:
                    np = body["new_pin"]
                    if np == "" or len(np) == 4:
                        state["pin"] = np
                if "tz_offset" in body:
                    state["tz_offset"] = body["tz_offset"]
                if "tz_dst" in body:
                    state["tz_dst"] = body["tz_dst"]
                if "geo_lat" in body:
                    state["geo_lat"] = body["geo_lat"]
                if "geo_lon" in body:
                    state["geo_lon"] = body["geo_lon"]
                if "rain_check" in body:
                    state["rain_check"] = body["rain_check"]
                if "tank_enabled" in body:
                    state["tank_enabled"] = body["tank_enabled"]
                if "tank_empty_cm" in body:
                    state["tank_empty_cm"] = body["tank_empty_cm"]
                if "tank_full_cm" in body:
                    state["tank_full_cm"] = body["tank_full_cm"]
                if "rain_pause_hours" in body:
                    h = body["rain_pause_hours"]
                    if h > 0:
                        state["rain_pause_until"] = time.time() + h * 3600
                        state["rain_paused"] = True
                        print(f"[Rain] Paused for {h}h")
                    else:
                        state["rain_pause_until"] = 0
                        state["rain_paused"] = False
                        print("[Rain] Resumed")
            self.json_response(status_json())

        elif self.path == "/api/zone/config":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            z = body.get("zone", -1)
            with lock:
                if 0 <= z < len(state["zones"]):
                    if "name" in body:
                        state["zones"][z]["name"] = body["name"]
                    if "seconds" in body:
                        s = body["seconds"]
                        if 1 <= s <= 600:
                            state["zones"][z]["seconds"] = s
            self.json_response(status_json())

        elif self.path == "/api/zone/schedules":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            z = body.get("zone", -1)
            with lock:
                if 0 <= z < len(state["zones"]):
                    state["zones"][z]["schedules"] = body.get("schedules", [])[:4]
            self.json_response(status_json())

        elif self.path == "/api/log/clear":
            if LOG_FILE.exists():
                LOG_FILE.unlink()
            print("[Log] Cleared")
            self.json_response({"ok": True})

        elif self.path == "/api/update/check":
            self.json_response({
                "version": "1.3.0",
                "update_version": state.get("update_version", ""),
                "update_notes": state.get("update_notes", ""),
                "update_available": bool(state.get("update_version")),
            })

        elif self.path == "/api/update/install":
            print("[Update] Mock install triggered — would download and reboot")
            self.json_response({"ok": True, "msg": "mock install"})

        elif self.path == "/api/ota":
            print("[OTA] Mock upload received (no actual update)")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            body = b"OK - mock reboot"
            self.send_header("Content-Length", len(body))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_error(404)

    def read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        if length:
            return json.loads(self.rfile.read(length))
        return {}

    def json_response(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def html_response(self, html):
        body = html.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, name, content_type):
        path = DATA_DIR / name
        if not path.exists():
            self.send_error(404)
            return
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", len(data))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        if "/api/status" not in args[0]:  # suppress poll spam
            print(f"[HTTP] {args[0]}")


if __name__ == "__main__":
    # Start auto-stop background thread
    t = threading.Thread(target=check_auto_stop, daemon=True)
    t.start()

    server = HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Mock server running at http://localhost:{PORT}")
    if SETUP_MODE:
        print("MODE: Setup (captive portal)")
        print("Simulates first-boot WiFi provisioning")
    else:
        print("MODE: Normal (zone control)")
        print("Fake zones: Front Yard, Back Patio, Side Yard, Garden")
        print("Fake peer: Neighbor's Yard (192.168.1.101)")
        print(f"Fake location: {state['geo_lat']}, {state['geo_lon']} (Austin, TX)")
    print("Ctrl+C to stop\n")
    server.serve_forever()
