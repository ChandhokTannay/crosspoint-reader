#!/usr/bin/env python3
import json
import os
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

BOOKS_ROOT = "books"
INCOMING_DIR = os.path.join(BOOKS_ROOT, "incoming")
ARCHIVE_DIR = os.path.join(BOOKS_ROOT, "archived")


def ensure_dirs():
    os.makedirs(INCOMING_DIR, exist_ok=True)
    os.makedirs(ARCHIVE_DIR, exist_ok=True)


def list_pending_books():
    ensure_dirs()
    books = []
    try:
        for name in sorted(os.listdir(INCOMING_DIR)):
            if not name.lower().endswith(".epub"):
                continue
            full = os.path.join(INCOMING_DIR, name)
            if not os.path.isfile(full):
                continue
            size = os.path.getsize(full)
            books.append({"name": name, "size": int(size)})
    except FileNotFoundError:
        pass
    return books


class BookHandler(BaseHTTPRequestHandler):
    def _set_headers(self, status=200, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.end_headers()

    def log_message(self, format, *args):  # noqa: D401
        """Silence default HTTP request logging."""
        return

    def do_GET(self):  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/books/pending":
            books = list_pending_books()
            self._set_headers(200)
            self.wfile.write(json.dumps({"ok": True, "books": books}).encode("utf-8"))
            return

        if parsed.path == "/books/download":
            qs = parse_qs(parsed.query)
            name = qs.get("name", [None])[0]
            if not name:
                self._set_headers(400)
                self.wfile.write(b'{"ok": false, "error": "missing name"}')
                return

            # Basic safety: only allow simple basenames, no path separators
            if os.path.sep in name or "/" in name or ".." in name:
                self._set_headers(400)
                self.wfile.write(b'{"ok": false, "error": "invalid name"}')
                return

            ensure_dirs()
            full = os.path.join(INCOMING_DIR, name)
            if (not full.lower().endswith(".epub")) or (not os.path.isfile(full)):
                self._set_headers(404)
                self.wfile.write(b'{"ok": false, "error": "book not found"}')
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/epub+zip")
            self.send_header("Content-Length", str(os.path.getsize(full)))
            self.end_headers()
            with open(full, "rb") as f:
                while True:
                    chunk = f.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        # Anything else
        self._set_headers(404)
        self.wfile.write(b'{"ok": false, "error": "not found"}')

    def do_POST(self):  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path != "/books/ack":
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

        name = payload.get("name")
        if not isinstance(name, str) or not name:
            self._set_headers(400)
            self.wfile.write(b'{"ok": false, "error": "name required"}')
            return

        if os.path.sep in name or "/" in name or ".." in name:
            self._set_headers(400)
            self.wfile.write(b'{"ok": false, "error": "invalid name"}')
            return

        ensure_dirs()
        src = os.path.join(INCOMING_DIR, name)
        dst = os.path.join(ARCHIVE_DIR, name)

        if os.path.isfile(src):
            try:
                os.replace(src, dst)
            except OSError:
                self._set_headers(500)
                self.wfile.write(b'{"ok": false, "error": "failed to move"}')
                return

        # If already archived or just moved, treat as success
        self._set_headers(200)
        self.wfile.write(b'{"ok": true}')


def run(host="0.0.0.0", port=8081):
    ensure_dirs()
    server = HTTPServer((host, port), BookHandler)
    print(f"Serving book delivery on {host}:{port}\nDrop .epub files into {INCOMING_DIR}.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    run()
