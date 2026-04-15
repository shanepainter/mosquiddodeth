#!/usr/bin/env python3
"""Mock ESP32 server for testing the MosquittoDeth web UI."""

import json
import time
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

DATA_DIR = Path(__file__).parent / "data"
PORT = 8080

# ── Fake device state ──
state = {
    "device_id": "a1b2",
    "hostname": "mosquitto-death-a1b2",
    "friendly_name": "",
    "pin": "",
    "active_zone": -1,
    "queue": [],
    "zones": [
        {"name": "Front Yard", "seconds": 90, "active": False, "start": 0, "pin": 26, "queued": False, "schedules": [
            {"enabled": True, "days": 0b0100100, "hour": 6, "minute": 0}
        ]},
        {"name": "Back Patio", "seconds": 120, "active": False, "start": 0, "pin": 25, "queued": False, "schedules": []},
        {"name": "Side Yard", "seconds": 60, "active": False, "start": 0, "pin": 33, "queued": False, "schedules": []},
        {"name": "Garden", "seconds": 45, "active": False, "start": 0, "pin": 32, "queued": False, "schedules": []},
    ],
}
lock = threading.Lock()


def check_auto_stop():
    """Background thread to auto-stop zones and process queue."""
    while True:
        with lock:
            az = state["active_zone"]
            if az >= 0:
                z = state["zones"][az]
                if z["active"] and time.time() - z["start"] >= z["seconds"]:
                    z["active"] = False
                    state["active_zone"] = -1
                    print(f"[Zone {az}] {z['name']} auto-stopped")
                    # Process queue
                    if state["queue"]:
                        nz = state["queue"].pop(0)
                        state["zones"][nz]["active"] = True
                        state["zones"][nz]["start"] = time.time()
                        state["active_zone"] = nz
                        print(f"[Zone {nz}] {state['zones'][nz]['name']} started from queue")
            # Update queued flags
            for i, z in enumerate(state["zones"]):
                z["queued"] = i in state["queue"]
        time.sleep(1)


def status_json():
    with lock:
        az = state["active_zone"]
        doc = {
            "device_id": state["device_id"],
            "hostname": state["hostname"],
            "friendly_name": state["friendly_name"],
            "pin_enabled": len(state["pin"]) > 0,
            "ip": "192.168.1.100",
            "uptime": int(time.time()) % 86400,
            "wifi_rssi": -52,
            "active_zone": az,
            "queue_size": len(state["queue"]),
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "weekday": int(time.strftime("%w")),
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


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.serve_file("index.html", "text/html")
        elif self.path == "/apple-touch-icon.png":
            self.serve_file("apple-touch-icon.png", "image/png")
        elif self.path == "/icon-512.png":
            self.serve_file("icon-512.png", "image/png")
        elif self.path == "/logo.png":
            self.serve_file("logo.png", "image/png")
        elif self.path == "/api/status":
            self.json_response(status_json())
        elif self.path == "/api/devices":
            self.json_response({"devices": [
                {"id": state["device_id"], "hostname": state["hostname"],
                 "name": state["friendly_name"], "ip": "192.168.1.100",
                 "active": state["active_zone"] >= 0, "self": True},
                {"id": "c3d4", "hostname": "mosquitto-death-c3d4",
                 "name": "Neighbor's Yard", "ip": "192.168.1.101",
                 "active": False, "self": False},
            ]})
        else:
            self.send_error(404)

    def do_POST(self):
        body = self.read_body()

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
                        state["active_zone"] = z
                        print(f"[Zone {z}] {state['zones'][z]['name']} started")
            self.json_response(status_json())

        elif self.path == "/api/zone/stop":
            if not check_pin(body):
                self.json_response({"error": "pin_required"}, 403)
                return
            with lock:
                if body.get("all"):
                    for z in state["zones"]:
                        z["active"] = False
                    state["active_zone"] = -1
                    state["queue"].clear()
                    print("[Zones] All stopped")
                else:
                    z = body.get("zone", -1)
                    if 0 <= z < len(state["zones"]):
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
    print("Fake zones: Front Yard, Back Patio, Side Yard, Garden")
    print("Fake peer device: Neighbor's Yard (192.168.1.101)")
    print("Ctrl+C to stop\n")
    server.serve_forever()
