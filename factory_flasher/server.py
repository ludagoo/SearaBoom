"""Local HTTP UI for the factory flasher. Uses esptool, not WebSerial."""
from __future__ import annotations

import json
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from factory_flasher.session import FactorySession

STATIC_DIR = Path(__file__).resolve().parent / "static"


def _read_static(name: str) -> tuple[bytes, str]:
    path = (STATIC_DIR / name).resolve()
    if not str(path).startswith(str(STATIC_DIR.resolve())) or not path.is_file():
        raise FileNotFoundError(name)
    suffix = path.suffix.lower()
    mime = {
        ".html": "text/html; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
    }.get(suffix, "application/octet-stream")
    return path.read_bytes(), mime


def make_handler(session: FactorySession):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args) -> None:
            return

        def _send(self, code: int, body: bytes, mime: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code: int, payload: dict) -> None:
            self._send(code, json.dumps(payload).encode("utf-8"), "application/json")

        def do_GET(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            if path in ("/", "/index.html"):
                body, mime = _read_static("index.html")
                self._send(200, body, mime)
                return
            if path == "/api/state":
                self._json(200, session.snapshot())
                return
            name = path.lstrip("/")
            try:
                body, mime = _read_static(name)
            except FileNotFoundError:
                self._json(404, {"error": "not found"})
                return
            self._send(200, body, mime)

        def do_POST(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            try:
                data = json.loads(raw.decode("utf-8") or "{}")
            except json.JSONDecodeError:
                self._json(400, {"error": "invalid json"})
                return
            if not isinstance(data, dict):
                data = {}
            if path == "/api/arm":
                armed = bool(data.get("armed"))
                self._json(200, session.arm(armed))
                return
            if path == "/api/cal/retry":
                self._json(200, session.request_cal_retry())
                return
            self._json(404, {"error": "not found"})

    return Handler


def serve(
    session: FactorySession,
    *,
    host: str = "127.0.0.1",
    port: int = 8765,
    open_browser: bool = True,
) -> ThreadingHTTPServer:
    httpd = ThreadingHTTPServer((host, port), make_handler(session))
    if open_browser:
        url = f"http://{host}:{port}/"
        try:
            webbrowser.open(url)
        except Exception:
            pass
    return httpd


def run_poll_loop(session: FactorySession, interval_s: float = 0.4) -> threading.Event:
    stop = threading.Event()

    def loop() -> None:
        while not stop.is_set():
            try:
                session.tick()
            except Exception:
                pass
            stop.wait(interval_s)

    thread = threading.Thread(target=loop, name="factory-watch", daemon=True)
    thread.start()
    return stop
