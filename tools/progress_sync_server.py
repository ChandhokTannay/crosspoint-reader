#!/usr/bin/env python3
import json
import os
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

DATA_FILE = "progress.json"


def load_data():
    if not os.path.exists(DATA_FILE):
        return {}
    try:
        with open(DATA_FILE, "r") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return {}


def save_data(data):
    with open(DATA_FILE, "w") as f:
        json.dump(data, f, indent=2)


class ProgressHandler(BaseHTTPRequestHandler):
    def _set_headers(self, status=200, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.end_headers()

    def log_message(self, format, *args):  # noqa: D401
        """Silence default HTTP request logging."""
        return

    def do_PUT(self):  # noqa: N802
        if self.path != "/syncs/progress":
            self._set_headers(404)
            self.wfile.write(b'{"ok": false, "error": "not found"}')
            return

        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length)
        try:
            payload = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError:
            self._set_headers(400)
            self.wfile.write(b'{"ok": false, "error": "invalid json"}')
            return

        document = payload.get("document")
        percentage = payload.get("percentage")
        if not isinstance(document, str) or percentage is None:
            self._set_headers(400)
            self.wfile.write(b'{"ok": false, "error": "document and percentage required"}')
            return

        data = load_data()
        entry = {
            "progress": float(payload.get("progress", float(percentage) / 100.0)),
            "percentage": float(percentage),
            "device": payload.get("device", "crosspoint-x4"),
            "timestamp": int(payload.get("timestamp", int(time.time()))),
        }
        # Optional detailed position, used for exact resume
        if "spine_index" in payload:
            entry["spine_index"] = int(payload["spine_index"])
        if "page" in payload:
            entry["page"] = int(payload["page"])

        data[document] = entry
        save_data(data)

        self._set_headers(200)
        self.wfile.write(b'{"ok": true}')

    def do_GET(self):  # noqa: N802
        if not self.path.startswith("/syncs/progress"):
            self._set_headers(404)
            self.wfile.write(b'{"ok": false, "error": "not found"}')
            return

        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        document = qs.get("document", [None])[0]

        data = load_data()
        if document:
            entry = data.get(document)
            if not entry:
                self._set_headers(404)
                self.wfile.write(b'{"ok": false, "error": "no progress for document"}')
                return
            resp = {"ok": True, "data": entry}
        else:
            resp = {"ok": True, "data": data}

        self._set_headers(200)
        self.wfile.write(json.dumps(resp).encode("utf-8"))


def run(host="0.0.0.0", port=8080):
    server = HTTPServer((host, port), ProgressHandler)
    print(f"Serving progress sync on {host}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    run()

