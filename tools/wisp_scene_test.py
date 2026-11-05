#!/usr/bin/env python3
"""Tests for tools/wisp_scene.py: packing rules, the token file, and the
admin API conversation against a fake device served from this process
(pairing, /api/v1 paths, re-pairing on 401, the nested error shape, the
scene log over a minimal WebSocket). The CLI runs as a subprocess, so
everything it prints is captured.

Run directly: python3 tools/wisp_scene_test.py
"""

import base64
import hashlib
import http.server
import io
import json
import os
import pathlib
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parent
SCRIPT = TOOLS / "wisp_scene.py"
sys.path.insert(0, str(TOOLS))

import wisp_scene  # noqa: E402

TOKEN = "fake-device-token-0123456789abcdef"
CODE = "482913"

# A minimal JPEG header: SOI, then a frame header (SOFn) of an 8x8 image.
def jpeg(sof):
    frame = bytes([0xFF, sof, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08, 0x01, 0x01, 0x11, 0x00])
    return b"\xFF\xD8" + frame + b"\xFF\xD9"


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    path.write_bytes(data)


def make_scene(root, scene_id="counter"):
    d = root / scene_id
    write(d / "scene.json", json.dumps({
        "id": scene_id, "name": "计数器", "version": "1.2.0", "schema_version": 1,
        "permissions": {"http": ["api.example.com"]}}))
    write(d / "main.lua", "return ui.screen {}\n")
    write(d / "lib" / "a.lua", "return {}\n")
    write(d / "lib" / "notes.txt", "not lua\n")
    write(d / "assets" / "x.png", b"\x89PNG\r\n")
    write(d / "README.md", "# counter\n")
    write(d / ".git" / "HEAD", "ref: refs/heads/main\n")
    write(d / "data" / "kv.log", '{"op":"put"}\n')
    return d


class FakeDevice:
    """The admin API, as far as the tool uses it. Records every request
    line; the pairing code is CODE and pairing hands out TOKEN."""

    def __init__(self):
        self.requests = []  # (method, path, headers)
        self.uploads = []
        self.token = TOKEN
        self.upload_error = None
        self.ws_frames = []
        device = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _body(self):
                n = int(self.headers.get("Content-Length") or 0)
                return self.rfile.read(n) if n else b""

            def _send(self, status, obj=None, headers=()):
                body = json.dumps(obj if obj is not None else {}).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                for k, v in headers:
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)

            def _authed(self):
                return self.headers.get("Cookie") == f"wisp_token={device.token}"

            def _record(self):
                device.requests.append((self.command, self.path, dict(self.headers)))

            def do_GET(self):
                self._record()
                if self.path == "/api/v1/ws":
                    return self._websocket()
                if not self._authed():
                    return self._send(401, {"error": {"code": "unauthorized",
                                                      "message": "pair this browser first"}})
                if self.path == "/api/v1/scenes":
                    return self._send(200, {"scenes": [{
                        "id": "counter", "name": "计数器", "version": "1.2.0",
                        "code_bytes": 120, "data_bytes": 14, "state": "running",
                        "hosts": ["api.example.com"], "error": None}]})
                self._send(404, {"error": {"code": "not_found", "message": "no"}})

            def do_POST(self):
                self._record()
                body = self._body()
                if self.headers.get("X-Wisp") != "1":
                    return self._send(403, {"error": {"code": "csrf", "message": "X-Wisp"}})
                if self.path == "/api/v1/pair/start":
                    return self._send(202, {})
                if self.path == "/api/v1/pair/confirm":
                    if json.loads(body).get("code") != CODE:
                        return self._send(403, {"error": {"code": "invalid_code",
                                                          "message": "wrong code"}})
                    return self._send(200, {"paired": True}, [(
                        "Set-Cookie",
                        f"wisp_token={device.token}; HttpOnly; SameSite=Strict; Path=/")])
                if not self._authed():
                    return self._send(401, {"error": {"code": "unauthorized",
                                                      "message": "pair this browser first"}})
                if self.path == "/api/v1/scenes":
                    if device.upload_error:
                        return self._send(400, {"error": device.upload_error})
                    device.uploads.append(body)
                    with zipfile.ZipFile(io.BytesIO(body)) as z:
                        m = json.loads(z.read("scene.json"))
                    return self._send(201, {
                        "id": m["id"], "name": m["name"], "version": m["version"],
                        "code_bytes": 1, "data_bytes": 0, "state": "stopped",
                        "hosts": m.get("permissions", {}).get("http", []), "error": None})
                if self.path.endswith("/open"):
                    return self._send(200, {"state": "running"})
                self._send(404, {"error": {"code": "not_found", "message": "no"}})

            def _websocket(self):
                if not self._authed():
                    return self._send(401, {"error": {"code": "unauthorized", "message": "x"}})
                key = self.headers["Sec-WebSocket-Key"]
                accept = base64.b64encode(hashlib.sha1(
                    (key + "258EAFA5-E914-47DA-95CA-C5AB0DC11B9A").encode()).digest()).decode()
                self.send_response(101)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", accept)
                self.end_headers()
                self.wfile.flush()
                # The client's one masked text frame.
                b0, b1 = self.rfile.read(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self.rfile.read(2))[0]
                mask = self.rfile.read(4)
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(n)))
                device.ws_frames.append(payload.decode())
                for msg in [
                    {"t": "hello", "boot_id": "b"},
                    {"t": "log", "seq": 1, "ts": 1, "level": "I", "tag": "scene.counter",
                     "msg": "show"},
                    {"t": "log", "seq": 2, "ts": 2, "level": "I", "tag": "scene.other",
                     "msg": "not mine"},
                    {"t": "log", "seq": 3, "ts": 3, "level": "E", "tag": "scene.counter",
                     "msg": "main.lua:3: boom" + "x" * 200},
                ]:
                    data = json.dumps(msg).encode()
                    head = bytes([0x81, len(data)]) if len(data) < 126 else \
                        bytes([0x81, 126]) + struct.pack(">H", len(data))
                    self.wfile.write(head + data)
                self.wfile.write(bytes([0x88, 0]))
                self.wfile.flush()
                self.close_connection = True

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def paths(self):
        return [p for _, p, _ in self.requests]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.config = self.tmp / "config"
        self.device = FakeDevice()
        self.host = f"127.0.0.1:{self.device.port}"

    def tearDown(self):
        self.device.close()
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_tool(self, *args, stdin=CODE + "\n"):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--host", self.host, "--config-dir", str(self.config),
             *args],
            input=stdin, capture_output=True, text=True, timeout=30)


class Packing(Base):
    def test_packs_only_allowed_files(self):
        d = make_scene(self.tmp)
        blob, manifest, skipped = wisp_scene.pack(d)
        self.assertEqual(manifest["id"], "counter")
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            names = sorted(z.namelist())
            self.assertTrue(all(i.compress_type == zipfile.ZIP_DEFLATED for i in z.infolist()))
        self.assertEqual(names, ["assets/x.png", "lib/a.lua", "main.lua", "scene.json"])
        self.assertIn("README.md", skipped)
        self.assertIn(".git/", skipped)
        self.assertIn("lib/notes.txt", skipped)
        # data/ belongs to the device's store: never sent, not worth a line.
        self.assertFalse(any(s.startswith("data") for s in skipped))

        proc = self.run_tool("push", str(d))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("已跳过", proc.stdout)
        self.assertIn("README.md", proc.stdout)
        self.assertIn(".git/", proc.stdout)
        self.assertEqual(len(self.device.uploads), 1)
        with zipfile.ZipFile(io.BytesIO(self.device.uploads[0])) as z:
            self.assertIn("lib/a.lua", z.namelist())
            self.assertIn("assets/x.png", z.namelist())
            self.assertNotIn("README.md", z.namelist())
        # id, version, declared hosts, state.
        self.assertIn("counter", proc.stdout)
        self.assertIn("1.2.0", proc.stdout)
        self.assertIn("api.example.com", proc.stdout)
        self.assertIn("stopped", proc.stdout)

    def test_rejects_progressive_jpeg(self):
        d = make_scene(self.tmp)
        write(d / "assets" / "p.jpg", jpeg(0xC2))
        with self.assertRaises(wisp_scene.ToolError) as cm:
            wisp_scene.pack(d)
        self.assertIn("assets/p.jpg", str(cm.exception))
        proc = self.run_tool("push", str(d))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("assets/p.jpg", proc.stderr)
        self.assertEqual(self.device.uploads, [])

        write(d / "assets" / "p.jpg", jpeg(0xC0))
        blob, _, _ = wisp_scene.pack(d)
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            self.assertIn("assets/p.jpg", z.namelist())

    def test_missing_manifest_is_an_error(self):
        d = self.tmp / "empty"
        d.mkdir()
        with self.assertRaises(wisp_scene.ToolError) as cm:
            wisp_scene.pack(d)
        self.assertIn("scene.json", str(cm.exception))

    def test_snapshot_sees_same_size_edits(self):
        d = make_scene(self.tmp)
        before = wisp_scene.snapshot(d)
        write(d / "main.lua", "return ui.screen {}\t")
        self.assertNotEqual(before, wisp_scene.snapshot(d))
        before = wisp_scene.snapshot(d)
        write(d / "data" / "kv.log", "changed\n")
        self.assertEqual(before, wisp_scene.snapshot(d))


class Tokens(Base):
    def test_token_file_is_0600_and_atomic(self):
        d = make_scene(self.tmp)
        proc = self.run_tool("push", str(d))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        path = self.config / "scene_tool.json"
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(json.loads(path.read_text())["hosts"][self.host]["token"], TOKEN)
        # Nothing but the file itself is left behind.
        self.assertEqual(sorted(p.name for p in self.config.iterdir()), ["scene_tool.json"])

        # Keyed by host: saving one keeps the others; the file is replaced
        # in one step (written aside, then os.replace()).
        store = wisp_scene.TokenStore(self.config)
        real_replace = os.replace
        calls = []

        def spy(src, dst):
            calls.append((src, dst))
            self.assertEqual(stat.S_IMODE(os.stat(src).st_mode), 0o600)
            return real_replace(src, dst)

        with mock.patch.object(wisp_scene.os, "replace", side_effect=spy):
            store.put("other.local:8900", "t2")
        self.assertEqual(len(calls), 1)
        self.assertEqual(pathlib.Path(calls[0][1]), path)
        self.assertNotEqual(pathlib.Path(calls[0][0]), path)
        hosts = json.loads(path.read_text())["hosts"]
        self.assertEqual(hosts[self.host]["token"], TOKEN)
        self.assertEqual(hosts["other.local:8900"]["token"], "t2")
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_token_never_printed(self):
        d = make_scene(self.tmp)
        outputs = []
        for args in (("push", str(d), "--open"), ("ls",), ("logs", "counter")):
            proc = self.run_tool(*args)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            outputs += [proc.stdout, proc.stderr]
        # A failed upload too.
        self.device.upload_error = {"code": "invalid_scene", "message": "main.lua:3: boom",
                                    "file": "main.lua", "line": 3}
        proc = self.run_tool("push", str(d))
        outputs += [proc.stdout, proc.stderr]
        for text in outputs:
            self.assertNotIn(TOKEN, text)
            self.assertNotIn(TOKEN[:12], text)

    def test_repairs_on_401(self):
        d = make_scene(self.tmp)
        wisp_scene.TokenStore(self.config).put(self.host, "stale-token")
        proc = self.run_tool("push", str(d))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.device.paths().count("/api/v1/pair/confirm"), 1)
        stored = json.loads((self.config / "scene_tool.json").read_text())
        self.assertEqual(stored["hosts"][self.host]["token"], TOKEN)

    def test_stored_token_is_reused(self):
        d = make_scene(self.tmp)
        self.assertEqual(self.run_tool("push", str(d)).returncode, 0)
        proc = self.run_tool("ls", stdin="")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.device.paths().count("/api/v1/pair/start"), 1)
        self.assertIn("counter", proc.stdout)
        self.assertIn("running", proc.stdout)


class Api(Base):
    def test_requests_use_api_prefix(self):
        d = make_scene(self.tmp)
        self.assertEqual(self.run_tool("push", str(d), "--open").returncode, 0)
        self.assertEqual(self.run_tool("ls").returncode, 0)
        self.assertEqual(self.run_tool("logs", "counter").returncode, 0)
        paths = self.device.paths()
        for p in paths:
            self.assertTrue(p.startswith("/api/v1/"), p)
        for want in ("/api/v1/pair/start", "/api/v1/pair/confirm", "/api/v1/scenes",
                     "/api/v1/scenes/counter/open", "/api/v1/ws"):
            self.assertIn(want, paths)
        for method, path, headers in self.device.requests:
            self.assertEqual(headers.get("X-Wisp"), "1", path)
            if not path.startswith("/api/v1/pair/"):
                self.assertEqual(headers.get("Cookie"), f"wisp_token={TOKEN}", path)

    def test_upload_error_names_file_and_line(self):
        d = make_scene(self.tmp)
        self.device.upload_error = {"code": "invalid_scene",
                                    "message": "main.lua:3: unexpected symbol",
                                    "file": "main.lua", "line": 3}
        proc = self.run_tool("push", str(d))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("main.lua:3", proc.stderr)
        self.assertIn("unexpected symbol", proc.stderr)
        self.assertIn("invalid_scene", proc.stderr)

    def test_logs_prints_only_the_scenes_lines(self):
        proc = self.run_tool("logs", "counter")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(self.device.ws_frames[0]), {"sub": ["log"]})
        self.assertIn("show", proc.stdout)
        self.assertIn("main.lua:3: boom", proc.stdout)
        self.assertNotIn("not mine", proc.stdout)

    def test_rejects_a_bad_id(self):
        proc = self.run_tool("logs", "../x")
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(self.device.requests, [])


class Defaults(unittest.TestCase):
    def test_config_dir(self):
        with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": "/x/cfg"}):
            self.assertEqual(wisp_scene.default_config_dir(), pathlib.Path("/x/cfg/wisp"))
        env = dict(os.environ)
        env.pop("XDG_CONFIG_HOME", None)
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(wisp_scene.default_config_dir(),
                             pathlib.Path.home() / ".config" / "wisp")
        self.assertEqual(wisp_scene.DEFAULT_HOST, "wisp.local:8900")


if __name__ == "__main__":
    unittest.main()
