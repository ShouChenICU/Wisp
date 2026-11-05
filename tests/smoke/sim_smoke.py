#!/usr/bin/env python3
"""Simulator smoke tests.

Runs the wisp_sim executable (simulator/CMakeLists.txt, built by the
host CMake project) through a handful of --headless scenarios and
checks its machine-readable stdout (the single-line-JSON
"WISP_BOOT_REPORT ", "WISP_MANIFEST " and "WISP_HTTP " lines -- see
AGENTS.md section 4 and docs/architecture.md) and, for the default
case, the PNG screenshot it writes. Each case runs wisp_sim in a fresh
temporary data directory. Every case except "http", "factory_reset",
"firmware" and "scene_push" passes --http-port 0 (no admin server), so the rest of
the suite never depends on a free port; those three pick one, talk to
the admin API over loopback and end the run with a reboot through it
("factory_reset" and "firmware" also listen on the admin WebSocket and
confirm on the screen with "tap" lines written to the simulator's
stdin). "console"
feeds serial console commands through stdin. The scene cases
(scene-runtime-core spec 9.2) run scenes/examples and
tests/scene/fixtures: "scene_counter" and "scene_hot_reload" click with
"debug scene-click" on stdin and follow the scene's log lines,
"scene_error" swipes back from a failed scene, and "scene_push" pushes a
scene with tools/wisp_scene.py (paired through "debug pair-code") and
deletes it, confirming on the screen; none of them reaches the network.
Every other case gives the simulator an empty stdin.

Registered with ctest as the "sim_smoke" test (tests/CMakeLists.txt).

Usage: python3 tests/smoke/sim_smoke.py <path-to-wisp_sim>
"""

import base64
import hashlib
import os
import http.client
import json
import re
import pathlib
import select
import socket
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback

TIMEOUT_S = 20
EXIT_AFTER_MS = 3000

# The 13 devices SimBoard::init() registers by default (see
# components/wisp_port_sim/sim_board.cpp).
EXPECTED_DEVICE_IDS = {
    "audio_in.main",
    "audio_out.main",
    "button.power",
    "button.vol_down",
    "button.vol_up",
    "clock.main",
    "display.main",
    "imu.main",
    "net.wifi",
    "power.main",
    "storage.int",
    "storage.sd",
    "touch.main",
}

# display.main's safe_area for the default shape (rounded_rect, corner
# radius 40) and for --shape circle (spec 5.6's safe-area model).
DEFAULT_SAFE_AREA = {"x1": 12, "y1": 12, "x2": 467, "y2": 467}
CIRCLE_SAFE_AREA = {"x1": 71, "y1": 71, "x2": 408, "y2": 408}

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class SmokeError(AssertionError):
    """Raised by a case function to fail that case with a message."""


def expect(condition, message):
    if not condition:
        raise SmokeError(message)


def sim_args(sim_path, data_dir, extra_args, http_port=0):
    return [
        str(sim_path),
        "--headless",
        "--exit-after-ms",
        str(EXIT_AFTER_MS),
        "--data-dir",
        str(data_dir),
        "--http-port",
        str(http_port),
        *extra_args,
    ]


def run_sim(sim_path, data_dir, extra_args, stdin_text=""):
    """stdin_text is the simulator's whole stdin (it then sees end of
    input), so a case never reads from the terminal ctest runs in."""
    args = sim_args(sim_path, data_dir, extra_args)
    try:
        return subprocess.run(args, input=stdin_text, capture_output=True, text=True,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"{' '.join(args)} timed out after {TIMEOUT_S}s") from exc


def parse_output(stdout):
    """Pulls the WISP_BOOT_REPORT/WISP_MANIFEST JSON payloads out of
    wisp_sim's stdout (one line each, prefix + single-line JSON). A
    WISP_HTTP line, if any, must be well-formed too."""
    boot_report = None
    manifest = None
    for line in stdout.splitlines():
        if line.startswith("WISP_BOOT_REPORT "):
            boot_report = json.loads(line[len("WISP_BOOT_REPORT "):])
        elif line.startswith("WISP_MANIFEST "):
            manifest = json.loads(line[len("WISP_MANIFEST "):])
        elif line.startswith("WISP_HTTP "):
            port = json.loads(line[len("WISP_HTTP "):]).get("port")
            expect(isinstance(port, int) and 0 < port < 65536, f"bad WISP_HTTP line: {line}")
    if boot_report is None:
        raise SmokeError(f"no WISP_BOOT_REPORT line in stdout:\n{stdout}")
    if manifest is None:
        raise SmokeError(f"no WISP_MANIFEST line in stdout:\n{stdout}")
    return boot_report, manifest


def devices_by_id(value):
    """value is a boot report or manifest dict; both carry a "devices"
    list of per-device dicts keyed by "id"."""
    return {entry["id"]: entry for entry in value["devices"]}


def expected_manifest_ids(boot_report):
    """Returns the device ids WISP_MANIFEST should carry for this run,
    given its boot report. Normally that's exactly EXPECTED_DEVICE_IDS,
    but ruling 1's audio_in.main leniency (ok-or-failed) has a
    manifest-side consequence: when SimBoard::init() can't find an SDL
    capture device it never calls registry.add() for audio_in.main at
    all (see sim_board.cpp), so a "failed" audio_in.main in the boot
    report means the manifest must NOT contain it either -- a strict
    12-id equality would wrongly fail on hosts without a capture
    device. A missing/ok audio_in.main entry (or no entry at all)
    leaves the full set untouched."""
    ids = set(EXPECTED_DEVICE_IDS)
    audio_in = devices_by_id(boot_report).get("audio_in.main")
    if audio_in is not None and audio_in["state"] == "failed":
        ids.discard("audio_in.main")
    return ids


def case_expected_manifest_ids_unit(_sim_path, _tmp_root):
    """Not a wisp_sim case: a synthetic-data unit check for
    expected_manifest_ids() itself, since case_default alone only ever
    exercises whichever one of its two branches this particular host
    happens to hit for audio_in.main."""
    ok_report = {"devices": [{"id": "audio_in.main", "state": "ok", "reason": ""}]}
    expect(
        expected_manifest_ids(ok_report) == EXPECTED_DEVICE_IDS,
        "expected_manifest_ids(): an ok audio_in.main must not change EXPECTED_DEVICE_IDS",
    )

    failed_report = {
        "devices": [{"id": "audio_in.main", "state": "failed", "reason": "no capture device"}]
    }
    expect(
        expected_manifest_ids(failed_report) == EXPECTED_DEVICE_IDS - {"audio_in.main"},
        "expected_manifest_ids(): a failed audio_in.main must drop it from the expected set",
    )

    no_entry_report = {"devices": []}
    expect(
        expected_manifest_ids(no_entry_report) == EXPECTED_DEVICE_IDS,
        "expected_manifest_ids(): no audio_in.main entry at all must not change EXPECTED_DEVICE_IDS",
    )


def read_png_chunks(path):
    """Returns (width, height, idat) for the PNG at `path`: idat is the
    concatenation of every IDAT chunk's data. Only walks the chunk
    structure (length + type + data + CRC) -- doesn't verify CRCs."""
    data = path.read_bytes()
    expect(data[:8] == PNG_SIGNATURE, f"{path}: missing PNG signature")

    pos = 8
    width = height = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8].decode("ascii", errors="replace")
        chunk_data = data[pos + 8:pos + 8 + length]
        if chunk_type == "IHDR":
            width, height = struct.unpack(">II", chunk_data[0:8])
        elif chunk_type == "IDAT":
            idat += chunk_data
        elif chunk_type == "IEND":
            break
        pos += 8 + length + 4  # length + type + data + CRC-32
    expect(width is not None, f"{path}: no IHDR chunk found")
    return width, height, bytes(idat)


def inflate_stored_blocks(zlib_data):
    """Decodes a zlib stream made only of uncompressed ("stored")
    deflate blocks (RFC 1950/1951) -- exactly what
    components/wisp_port_sim/png_writer.cpp emits: it only ever writes
    stored blocks, never a real deflate compressor, so this only needs
    to skip the zlib header and walk the blocks, not actually inflate
    anything. Skips the 2-byte zlib header, then walks each stored block's
    1-byte header
    (final bit + BTYPE) and 4-byte length/~length pair."""
    pos = 2  # CMF, FLG
    raw = bytearray()
    while pos < len(zlib_data):
        block_header = zlib_data[pos]
        is_final = block_header & 0x1
        pos += 1
        block_len = struct.unpack("<H", zlib_data[pos:pos + 2])[0]
        pos += 4  # LEN (2 bytes) + NLEN (2 bytes, ~LEN)
        raw += zlib_data[pos:pos + block_len]
        pos += block_len
        if is_final:
            break
    return bytes(raw)


def check_screenshot(path):
    expect(path.exists(), f"{path}: screenshot was not written")
    size = path.stat().st_size
    expect(size > 1024, f"{path}: screenshot is only {size} bytes, expected > 1 KiB")

    width, height, idat = read_png_chunks(path)
    expect((width, height) == (480, 480), f"{path}: expected 480x480, got {width}x{height}")

    pixels = inflate_stored_blocks(idat)
    expect(len(set(pixels)) > 1, f"{path}: decoded pixel bytes are all identical")


def case_default(sim_path, tmp_root):
    data_dir = tmp_root / "default"
    data_dir.mkdir()
    screenshot = data_dir / "s.png"

    result = run_sim(sim_path, data_dir, ["--screenshot", str(screenshot)])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, manifest = parse_output(result.stdout)
    expect(manifest["board"] == "simulator", f"manifest board == {manifest['board']!r}")

    manifest_devices = devices_by_id(manifest)
    manifest_ids = set(manifest_devices.keys())
    # audio_in.main is only in the manifest if the boot report didn't
    # just say it's "failed" -- see expected_manifest_ids()'s comment.
    expected_ids = expected_manifest_ids(boot_report)
    expect(
        manifest_ids == expected_ids,
        f"manifest device ids {sorted(manifest_ids)} != expected {sorted(expected_ids)}",
    )

    imu_caps = manifest_devices["imu.main"]["caps"]
    expect(
        imu_caps.get("magnetometer") is False,
        f"imu.main.caps.magnetometer == {imu_caps.get('magnetometer')!r}, expected false",
    )

    display_safe_area = manifest_devices["display.main"]["safe_area"]
    expect(
        display_safe_area == DEFAULT_SAFE_AREA,
        f"display.main safe_area == {display_safe_area}, expected {DEFAULT_SAFE_AREA}",
    )

    # All devices report "ok", with two controller-approved exceptions:
    # storage.sd (not asserted here; --no-sd covers its "missing"
    # state below) and audio_in.main, whose SDL dummy-driver capture
    # availability differs between hosts and so may legitimately come
    # back "failed" (with a reason) instead of "ok".
    report_devices = devices_by_id(boot_report)
    for device_id, entry in report_devices.items():
        if device_id == "storage.sd":
            continue
        if device_id == "audio_in.main":
            expect(
                entry["state"] in ("ok", "failed"),
                f"audio_in.main state == {entry['state']!r}, expected ok or failed",
            )
            if entry["state"] == "failed":
                expect(entry["reason"], "audio_in.main is failed with an empty reason")
            continue
        expect(
            entry["state"] == "ok",
            f"{device_id} state == {entry['state']!r}, expected ok",
        )

    # connectivity spec 4.1: a fresh data dir has no saved network.
    net_note = boot_report.get("notes", {}).get("net")
    expect(net_note == "off", f"notes.net == {net_note!r}, expected 'off'")
    # --http-port 0: no admin server, so no WISP_HTTP line.
    expect(
        not any(line.startswith("WISP_HTTP ") for line in result.stdout.splitlines()),
        "WISP_HTTP printed although --http-port 0 disables the admin server",
    )

    check_screenshot(screenshot)


def case_disable_wifi(sim_path, tmp_root):
    data_dir = tmp_root / "disable-wifi"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--disable", "net.wifi"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )
    boot_report, manifest = parse_output(result.stdout)
    net_note = boot_report.get("notes", {}).get("net")
    expect(net_note == "missing", f"notes.net == {net_note!r}, expected 'missing'")
    expect("net.wifi" not in devices_by_id(manifest), "net.wifi in the manifest despite --disable")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def api_request(port, method, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, path, headers=headers or {})
        res = conn.getresponse()
        return res.status, res.read().decode("utf-8")
    finally:
        conn.close()


def seed_paired_client(data_dir, token, label):
    """Pairs a browser by seeding net/clients.json with the SHA-256 of a
    known token, which is all the device stores."""
    digest = hashlib.sha256(token.encode()).hexdigest()
    clients = data_dir / "int" / "system" / "net" / "clients.json"
    clients.parent.mkdir(parents=True)
    clients.write_text(json.dumps({"clients": [
        {"id": digest[:12], "hash": digest, "label": label, "created": 0, "last_seen": 0},
    ]}))
    return clients


class WsClient:
    """Just enough of RFC 6455 for the admin socket: one handshake, masked
    text frames out, unmasked text/close frames in."""

    def __init__(self, port, cookie):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((
            "GET /api/v1/ws HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            f"Cookie: {cookie}\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            expect(chunk, f"socket closed during the WebSocket handshake: {head!r}")
            head += chunk
        expect(head.startswith(b"HTTP/1.1 101"), f"WebSocket upgrade refused: {head!r}")

    def _read_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def send_text(self, text):
        payload = text.encode()
        expect(len(payload) < 126, "WsClient only sends short frames")
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)

    def recv_text(self):
        """The next text message, or None once the device closes."""
        while True:
            head = self._read_exact(2)
            if head is None:
                return None
            opcode, size = head[0] & 0x0F, head[1] & 0x7F
            if size == 126:
                size = struct.unpack(">H", self._read_exact(2))[0]
            elif size == 127:
                size = struct.unpack(">Q", self._read_exact(8))[0]
            body = self._read_exact(size)
            if body is None or opcode == 0x8:
                return None
            if opcode == 0x1:
                return body.decode("utf-8")

    def close(self):
        self.sock.close()


class Child:
    """A child process whose stdout lines are collected on a thread (so a
    chatty log never fills the pipe) and can be waited on."""

    def __init__(self, args, stdin=False):
        self.proc = subprocess.Popen(
            args, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = []
        self.err_lines = []
        self.cond = threading.Condition()
        self.threads = [
            threading.Thread(target=self._drain, args=(self.proc.stdout, self.lines), daemon=True),
            threading.Thread(target=self._drain, args=(self.proc.stderr, self.err_lines),
                             daemon=True),
        ]
        for t in self.threads:
            t.start()

    def _drain(self, stream, sink):
        for line in stream:
            with self.cond:
                sink.append(line.rstrip("\n"))
                self.cond.notify_all()
        with self.cond:
            self.cond.notify_all()

    def wait_line(self, match, timeout_s, start=0):
        """The first stdout line from index `start` on for which match()
        is truthy, or None once timeout_s passes or the process exits."""
        deadline = time.monotonic() + timeout_s
        with self.cond:
            i = start
            while True:
                while i < len(self.lines):
                    if match(self.lines[i]):
                        return self.lines[i]
                    i += 1
                remaining = deadline - time.monotonic()
                if remaining <= 0 or (self.proc.poll() is not None
                                      and not any(t.is_alive() for t in self.threads)):
                    return None
                self.cond.wait(min(remaining, 0.2))

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def join_output(self):
        """Waits for the collector threads to read the process's last
        output; call only once the process has exited."""
        for t in self.threads:
            t.join(timeout=TIMEOUT_S)
            expect(not t.is_alive(), "the output pipes did not close after the process exited")

    def output(self):
        with self.cond:
            return "\n".join(self.lines), "\n".join(self.err_lines)

    def tail(self, n=20):
        with self.cond:
            return "\n".join(self.lines[-n:])

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        for t in self.threads:
            t.join(timeout=5)
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass


def case_http(sim_path, tmp_root):
    """The admin server comes up on the requested loopback port at boot
    (Platform::http_always_on), announces it with WISP_HTTP, answers the
    API, and POST /system/reboot ends the process with WISP_REBOOT and
    exit code 0. The browser is paired with seed_paired_client()."""
    data_dir = tmp_root / "http"
    token = "smoke-test-token"
    seed_paired_client(data_dir, token, "smoke")

    port = free_port()
    args = sim_args(sim_path, data_dir, [], http_port=port)
    # Long enough that only the reboot can end the run in time.
    args[args.index("--exit-after-ms") + 1] = str(TIMEOUT_S * 1000)
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        http_line = None
        seen = []
        for line in proc.stdout:
            seen.append(line)
            if line.startswith("WISP_HTTP "):
                http_line = json.loads(line[len("WISP_HTTP "):])
                break
        expect(http_line == {"port": port}, f"WISP_HTTP == {http_line!r}, stdout:\n{''.join(seen)}")

        status, body = api_request(port, "GET", "/api/v1/info")
        expect(status == 200, f"GET /api/v1/info -> {status} {body}")
        info = json.loads(body)
        # The build's version (cmake/wisp_version.cmake): X.Y.Z, then
        # optionally -N-gHASH and -dirty.
        expect(re.fullmatch(r"\d+\.\d+\.\d+(-\d+-g[0-9a-f]+)?(-dirty)?", str(info.get("fw")))
               is not None and info.get("paired") is False, f"/info == {info}")

        auth = {"Cookie": f"wisp_token={token}"}
        status, body = api_request(port, "GET", "/api/v1/device", auth)
        expect(status == 200, f"GET /api/v1/device -> {status} {body}")
        device = json.loads(body)
        expect(device["boot"]["notes"].get("net") == "off", f"/device boot notes: {device['boot']}")

        status, body = api_request(port, "POST", "/api/v1/system/reboot", {**auth, "X-Wisp": "1"})
        expect(status == 202, f"POST /api/v1/system/reboot -> {status} {body}")
        rest, err = proc.communicate(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise SmokeError("wisp_sim did not exit after POST /system/reboot") from exc
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
    expect("WISP_REBOOT" in rest.splitlines(), f"no WISP_REBOOT line after reboot:\n{rest}")
    expect(proc.returncode == 0, f"exit code {proc.returncode} after reboot, stderr:\n{err}")


# The reset dialog's "Yes" button, in English on the default 480x480
# rounded_rect screen. The dialog is make_confirm_box() in
# components/wisp_app/src/self_check_screen.cpp, centred in the safe area;
# move this when that layout or the "Yes" text changes.
RESET_YES_TAP = (188, 298)
# Logged by the self-check screen once the reset dialog is laid out.
RESET_DIALOG_LOG = "factory reset: waiting for confirmation on the screen"


def case_factory_reset(sim_path, tmp_root):
    """POST /system/factory-reset puts a confirmation dialog on the device;
    a tap on its "Yes" button resets. The browser's socket hears
    {state:"done"} before its token is revoked, the pairing and the
    settings are gone afterwards, and the process ends with WISP_REBOOT
    and exit code 0.

    Nothing here waits on a fixed delay: the socket subscribes to the log
    as well as to events, so the replayed log shows the subscription is
    live before the POST, and the dialog's own log line says when to tap
    (a "tap" line on the simulator's stdin). The POST goes out as
    soon as the server answers, typically before the boot has finished
    and the screen is listening: the request must still reach the dialog
    (the app keeps it pending until the screen takes it up)."""
    data_dir = tmp_root / "factory-reset"
    token = "smoke-reset-token"
    clients = seed_paired_client(data_dir, token, "smoke")
    # English, so the "Yes" button is where RESET_YES_TAP points.
    (data_dir / "settings.json").write_text(json.dumps({"sys": {"lang": "en"}}))

    port = free_port()
    args = sim_args(sim_path, data_dir, [], http_port=port)
    args[args.index("--exit-after-ms") + 1] = str(TIMEOUT_S * 1000)
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    # Drained on a thread from the start, so a chatty log can never fill
    # the pipe and stall the simulator while this waits on the socket.
    stdout_lines = []
    http_ready = threading.Event()

    def drain():
        for line in proc.stdout:
            stdout_lines.append(line)
            if line.startswith("WISP_HTTP "):
                http_ready.set()

    drainer = threading.Thread(target=drain, daemon=True)
    drainer.start()
    ws = None
    states = []
    tapped_at = None
    deadline = time.monotonic() + TIMEOUT_S
    try:
        expect(http_ready.wait(TIMEOUT_S), f"no WISP_HTTP line:\n{''.join(stdout_lines)}")
        cookie = f"wisp_token={token}"
        ws = WsClient(port, cookie)
        ws.sock.settimeout(TIMEOUT_S)
        ws.send_text(json.dumps({"sub": ["events", "log"]}))
        while True:
            text = ws.recv_text()
            expect(text is not None, "the device closed the socket before replaying its log")
            if json.loads(text).get("t") == "log":
                break

        status, body = api_request(port, "POST", "/api/v1/system/factory-reset",
                                   {"Cookie": cookie, "X-Wisp": "1"})
        expect(status == 202, f"POST /api/v1/system/factory-reset -> {status} {body}")

        while True:
            expect(time.monotonic() < deadline, f"no reset within {TIMEOUT_S}s; states {states}")
            expect(tapped_at is None or "done" in states or time.monotonic() - tapped_at < 5,
                   f"tapped {RESET_YES_TAP} but the reset did not start: the dialog's Yes "
                   f"button is not there any more (see RESET_YES_TAP); states {states}")
            # Wait for a frame in short steps, so the checks above run even
            # while the device sends nothing.
            if not select.select([ws.sock], [], [], 0.5)[0]:
                continue
            text = ws.recv_text()
            if text is None:
                break
            msg = json.loads(text)
            if msg.get("t") == "event" and msg.get("topic") == "system.factory_reset":
                states.append(msg["payload"].get("state"))
            elif (msg.get("t") == "log" and tapped_at is None
                  and RESET_DIALOG_LOG in msg.get("msg", "")):
                proc.stdin.write("tap {},{}\n".format(*RESET_YES_TAP))
                proc.stdin.flush()
                tapped_at = time.monotonic()
        expect(tapped_at is not None, f"the reset dialog never opened; states {states}")
        proc.wait(timeout=TIMEOUT_S)
        drainer.join(timeout=TIMEOUT_S)
        err = proc.stderr.read()
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"wisp_sim did not exit after the factory reset; states {states}") from exc
    except OSError as exc:
        # socket.timeout (TimeoutError), BrokenPipeError on the sim's stdin,
        # a reset or refused connection: say where the case was.
        step = ("waiting for the reset after the tap" if tapped_at is not None
                else "waiting for the reset dialog" if states else "before the POST was answered")
        tail = "".join(stdout_lines[-15:])
        raise SmokeError(f"{type(exc).__name__} {step}: {exc}; states {states}; "
                         f"wisp_sim exit code {proc.poll()}; last output:\n{tail}") from exc
    finally:
        if ws is not None:
            ws.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stdin.close()
    rest = "".join(stdout_lines)
    expect(states == ["requested", "done"], f"system.factory_reset states over the socket: {states}")
    expect("WISP_REBOOT" in rest.splitlines(), f"no WISP_REBOOT line after the reset:\n{rest}")
    expect(proc.returncode == 0, f"exit code {proc.returncode} after the reset, stderr:\n{err}")
    expect(json.loads(clients.read_text()).get("clients") == [],
           f"clients.json after the reset: {clients.read_text()}")
    settings = json.loads((data_dir / "settings.json").read_text())
    expect("lang" not in settings.get("sys", {}), f"settings.json after the reset: {settings}")


def make_firmware_image(version, board="simulator", project="wisp", chip_id=9):
    """A minimal ESP application image the way the firmware upload checks
    one (components/wisp_core/include/wisp/core/firmware_image.hpp): a
    header, esp_app_desc_t followed by Wisp's board descriptor in the
    first segment, a filler second segment, the checksum byte and the
    appended SHA-256. The simulator accepts any chip id."""
    seg0 = bytearray(256 + 48)
    struct.pack_into("<I", seg0, 0, 0xABCD5432)
    seg0[16:16 + len(version)] = version.encode()
    seg0[48:48 + len(project)] = project.encode()
    struct.pack_into("<IHH", seg0, 256, 0x42505357, 1, 0)  # "WSPB", version 1
    seg0[264:264 + len(board)] = board.encode()
    seg1 = bytes((i * 7 + 3) & 0xFF for i in range(4096))
    img = bytearray(24)
    img[0:4] = bytes((0xE9, 2, 2, 0x4F))  # magic, 2 segments, DIO, 80 MHz/16 MB
    struct.pack_into("<I", img, 4, 0x40370000)
    struct.pack_into("<H", img, 12, chip_id)
    img[23] = 1  # hash appended
    checksum, load = 0xEF, 0x3C000020
    for seg in (seg0, seg1):
        img += struct.pack("<II", load, len(seg)) + seg
        for b in seg:
            checksum ^= b
        load += len(seg)
    while (len(img) + 1) % 16:
        img.append(0)
    img.append(checksum)
    img += hashlib.sha256(img).digest()
    return bytes(img)


def post_firmware(port, cookie, image):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        conn.request("POST", "/api/v1/system/firmware", body=image, headers={
            "Cookie": cookie, "X-Wisp": "1", "Content-Type": "application/octet-stream"})
        res = conn.getresponse()
        return res.status, res.read().decode("utf-8")
    finally:
        conn.close()


# The firmware dialog's "Yes" button, in English on the default 480x480
# rounded_rect screen. The dialog is make_confirm_box() like the reset
# dialog; its text is the prompt, the running version and "-> new", one
# version per line (SelfCheck::show_firmware_dialog()). A git-described
# version fits one line, and FIRMWARE_VERSION is newer than any version
# this repository will reach, so no downgrade line is added: the layout
# does not depend on the checkout. Move this when that layout or the
# "Yes" text changes.
FIRMWARE_YES_TAP = (188, 307)
FIRMWARE_VERSION = "999.0.0"
FIRMWARE_DIALOG_LOG = f"firmware {FIRMWARE_VERSION}: waiting for confirmation on the screen"


def case_firmware(sim_path, tmp_root):
    """Uploads firmware the way the admin page does (spec 6.5): an image
    for another board is refused with 400 wrong_board (and a "failed"
    event) and leaves nothing behind; one for the simulator is answered 202, the socket hears
    "received", the dialog appears on the device and a tap on "Yes" gets
    "confirmed", WISP_REBOOT and exit code 0, with the image written to
    the data directory's firmware.bin (SimFirmwareUpdater)."""
    data_dir = tmp_root / "firmware"
    token = "smoke-firmware-token"
    seed_paired_client(data_dir, token, "smoke")
    (data_dir / "settings.json").write_text(json.dumps({"sys": {"lang": "en"}}))
    image = make_firmware_image(FIRMWARE_VERSION)

    port = free_port()
    args = sim_args(sim_path, data_dir, [], http_port=port)
    args[args.index("--exit-after-ms") + 1] = str(TIMEOUT_S * 1000)
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    stdout_lines = []
    http_ready = threading.Event()

    def drain():
        for line in proc.stdout:
            stdout_lines.append(line)
            if line.startswith("WISP_HTTP "):
                http_ready.set()

    drainer = threading.Thread(target=drain, daemon=True)
    drainer.start()
    ws = None
    states = []
    tapped_at = None
    deadline = time.monotonic() + TIMEOUT_S
    try:
        expect(http_ready.wait(TIMEOUT_S), f"no WISP_HTTP line:\n{''.join(stdout_lines)}")
        cookie = f"wisp_token={token}"
        ws = WsClient(port, cookie)
        ws.sock.settimeout(TIMEOUT_S)
        ws.send_text(json.dumps({"sub": ["events", "log"]}))
        while True:
            text = ws.recv_text()
            expect(text is not None, "the device closed the socket before replaying its log")
            if json.loads(text).get("t") == "log":
                break

        status, body = post_firmware(port, cookie,
                                     make_firmware_image(FIRMWARE_VERSION, board="other"))
        expect(status == 400 and json.loads(body)["error"]["code"] == "wrong_board",
               f"POST another board's image -> {status} {body}")
        expect(not (data_dir / "firmware.bin").exists(), "a refused image was kept")

        status, body = post_firmware(port, cookie, image)
        expect(status == 202 and json.loads(body) == {"version": FIRMWARE_VERSION},
               f"POST /api/v1/system/firmware -> {status} {body}")

        while True:
            expect(time.monotonic() < deadline, f"no restart within {TIMEOUT_S}s; states {states}")
            expect(tapped_at is None or "confirmed" in states or time.monotonic() - tapped_at < 5,
                   f"tapped {FIRMWARE_YES_TAP} but the update was not confirmed: the dialog's "
                   f"Yes button is not there any more (see FIRMWARE_YES_TAP); states {states}")
            if not select.select([ws.sock], [], [], 0.5)[0]:
                continue
            text = ws.recv_text()
            if text is None:
                break
            msg = json.loads(text)
            if msg.get("t") == "event" and msg.get("topic") == "system.firmware":
                expect(msg["payload"].get("version") == FIRMWARE_VERSION, f"event {msg}")
                state = msg["payload"].get("state")
                states.append(state if state != "failed" else
                              f"failed:{msg['payload'].get('error')}")
            elif (msg.get("t") == "log" and tapped_at is None
                  and FIRMWARE_DIALOG_LOG in msg.get("msg", "")):
                proc.stdin.write("tap {},{}\n".format(*FIRMWARE_YES_TAP))
                proc.stdin.flush()
                tapped_at = time.monotonic()
        expect(tapped_at is not None, f"the firmware dialog never opened; states {states}")
        proc.wait(timeout=TIMEOUT_S)
        drainer.join(timeout=TIMEOUT_S)
        err = proc.stderr.read()
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"wisp_sim did not exit after the update; states {states}") from exc
    except OSError as exc:
        tail = "".join(stdout_lines[-15:])
        raise SmokeError(f"{type(exc).__name__}: {exc}; states {states}; tapped {tapped_at is not None}; "
                         f"wisp_sim exit code {proc.poll()}; last output:\n{tail}") from exc
    finally:
        if ws is not None:
            ws.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stdin.close()
    rest = "".join(stdout_lines)
    expect(states == ["failed:wrong_board", "received", "confirmed"],
           f"system.firmware states over the socket: {states}")
    expect("WISP_REBOOT" in rest.splitlines(), f"no WISP_REBOOT line after the update:\n{rest}")
    expect(proc.returncode == 0, f"exit code {proc.returncode} after the update, stderr:\n{err}")
    written = data_dir / "firmware.bin"
    expect(written.is_file() and written.read_bytes() == image,
           f"firmware.bin after the update: {written.stat().st_size if written.exists() else None}"
           f" bytes, expected the {len(image)}-byte upload")


CONSOLE_PASSWORD = "wisp1234"


def case_console(sim_path, tmp_root):
    """Serial console commands on stdin (connectivity spec 8.2) are
    answered on stdout, touch lines never reach the console, and neither
    the Wi-Fi password typed into "wifi add" nor anything else secret is
    ever printed or logged."""
    commands = [
        "help",
        "wifi status",
        f"wifi add WispLab {CONSOLE_PASSWORD}",
        "tap 10,10",
        "wifi status",
        "wifi ap on",
        "wifi ap off",
        "bogus",
    ]
    result = run_sim(sim_path, tmp_root / "console", [], "\n".join(commands) + "\n")
    expect(result.returncode == 0, f"exit code {result.returncode}, stderr:\n{result.stderr}")
    lines = result.stdout.splitlines()
    expect('saved ssid="WispLab"' in lines, f"no saved network line:\n{result.stdout}")
    expect(any(l.startswith("state=") for l in lines), f"no wifi status line:\n{result.stdout}")
    expect(any(l.startswith('ap ssid="Wisp-') and " ip=" in l for l in lines),
           f"no hotspot line:\n{result.stdout}")
    expect(lines.count("ok") == 2, f"expected two ok lines:\n{result.stdout}")
    # Only "bogus" is unknown: "tap 10,10" went to the touch screen.
    expect(lines.count("error: unknown command") == 1,
           f"expected one unknown command:\n{result.stdout}")
    for stream in (result.stdout, result.stderr):
        expect(CONSOLE_PASSWORD not in stream, f"the Wi-Fi password was printed:\n{stream}")


def case_disable_imu(sim_path, tmp_root):
    data_dir = tmp_root / "disable-imu"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--disable", "imu"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, manifest = parse_output(result.stdout)
    report_devices = devices_by_id(boot_report)
    expect("imu.main" in report_devices, "imu.main missing from boot report")
    expect(
        report_devices["imu.main"]["state"] == "failed",
        f"imu.main state == {report_devices['imu.main']['state']!r}, expected failed",
    )
    expect(
        "--disable" in report_devices["imu.main"]["reason"],
        f"imu.main reason {report_devices['imu.main']['reason']!r} doesn't mention --disable",
    )

    expect(
        "imu.main" not in devices_by_id(manifest),
        "imu.main is present in the manifest despite --disable imu",
    )


def case_no_sd(sim_path, tmp_root):
    data_dir = tmp_root / "no-sd"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--no-sd"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, _manifest = parse_output(result.stdout)
    report_devices = devices_by_id(boot_report)
    expect("storage.sd" in report_devices, "storage.sd missing from boot report")
    expect(
        report_devices["storage.sd"]["state"] == "missing",
        f"storage.sd state == {report_devices['storage.sd']['state']!r}, expected missing",
    )

    # spec 9.2: "--no-sd 时 Library 只有内置根" -- boot_report.hpp's
    # "notes"."library_roots" is what makes that machine-readable rather
    # than only checkable by a unit test.
    library_roots = boot_report.get("notes", {}).get("library_roots")
    expect(library_roots is not None, "notes.library_roots missing from boot report")
    expect(
        library_roots == [str(data_dir / "int" / "library")],
        f"notes.library_roots == {library_roots!r}, expected only the internal root",
    )


def case_shape_circle(sim_path, tmp_root):
    data_dir = tmp_root / "shape-circle"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--shape", "circle"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    _boot_report, manifest = parse_output(result.stdout)
    display_safe_area = devices_by_id(manifest)["display.main"]["safe_area"]
    expect(
        display_safe_area == CIRCLE_SAFE_AREA,
        f"display.main safe_area == {display_safe_area}, expected {CIRCLE_SAFE_AREA}",
    )


# --- scenes (scene-runtime-core spec 9.2) ------------------------------------

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "scenes" / "examples"
SCENE_FIXTURES = ROOT / "tests" / "scene" / "fixtures"
WISP_SCENE = ROOT / "tools" / "wisp_scene.py"
# The simulator's log lines on stdout: "<ms> <level> <tag>: <message>".
LOG_LINE = re.compile(r"^\d+ [DIWE] ([^:]+): (.*)$")


def log_match(tag, message):
    """A Child.wait_line() matcher for the log line `tag`: `message`."""
    def match(line):
        m = LOG_LINE.match(line)
        return m is not None and m.group(1) == tag and m.group(2) == message
    return match


def scene_sim(sim_path, data_dir, extra_args, http_port=0):
    """A wisp_sim that only a signal (or the backstop) ends."""
    args = sim_args(sim_path, data_dir, extra_args, http_port=http_port)
    args[args.index("--exit-after-ms") + 1] = str(TIMEOUT_S * 1000)
    return Child(args, stdin=True)


def wait_log(sim, tag, message, start=0):
    """Index of the first log line `tag`: `message` from `start` on."""
    match = log_match(tag, message)
    line = sim.wait_line(match, TIMEOUT_S, start=start)
    expect(line is not None, f"no log line '{tag}: {message}'; last output:\n{sim.tail()}")
    with sim.cond:
        return sim.lines.index(line, start)


def click(sim, ref):
    """Clicks the widget `ref` of the scene in front. A scene logs "running"
    before the UI thread has shown its view and built its widgets, so "no
    scene is running" and "no widget with that ref" are asked again until
    the widget is there."""
    deadline = time.monotonic() + TIMEOUT_S
    while True:
        mark = len(sim.lines)
        sim.send(f"debug scene-click {ref}")
        line = sim.wait_line(lambda l: l.startswith("scene-click="), TIMEOUT_S, start=mark)
        if line == "scene-click=ok":
            return
        expect(line in ("scene-click=no widget with that ref", "scene-click=no scene is running")
               and time.monotonic() < deadline,
               f"debug scene-click {ref}: {line!r}")


def stop_child(sim):
    """SIGTERM, which must end the simulator cleanly."""
    sim.proc.send_signal(signal.SIGTERM)
    try:
        sim.proc.wait(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"wisp_sim ignored SIGTERM:\n{sim.tail()}") from exc
    sim.join_output()
    expect(sim.proc.returncode == 0, f"exit code {sim.proc.returncode} after SIGTERM:\n"
           f"{sim.output()[1]}")


def copy_examples(tmp_root, name):
    scenes = tmp_root / name / "scenes"
    shutil.copytree(EXAMPLES, scenes)
    return scenes


def case_scene_counter(sim_path, tmp_root):
    """The counter example from a scenes directory: two clicks on its "plus"
    button, each logged by the scene with its new count."""
    scenes = copy_examples(tmp_root, "scene-counter")
    sim = scene_sim(sim_path, tmp_root / "scene-counter" / "data",
                    ["--scenes-dir", str(scenes), "--open-scene", "counter"])
    try:
        at = wait_log(sim, "scene", "counter: running")
        click(sim, "plus")
        at = wait_log(sim, "scene.counter", "count=1", at)
        click(sim, "plus")
        wait_log(sim, "scene.counter", "count=2", at)
        stop_child(sim)
    finally:
        sim.kill()


def case_scene_hot_reload(sim_path, tmp_root):
    """Saving main.lua reloads the scene in front; its data stays."""
    scenes = copy_examples(tmp_root, "scene-hot-reload")
    sim = scene_sim(sim_path, tmp_root / "scene-hot-reload" / "data",
                    ["--scenes-dir", str(scenes), "--open-scene", "counter"])
    try:
        at = wait_log(sim, "scene", "counter: running")
        click(sim, "plus")
        at = wait_log(sim, "scene.counter", "count=1", at)
        main_lua = scenes / "counter" / "main.lua"
        main_lua.write_text('wisp.log.info("v2")\n' + main_lua.read_text(encoding="utf-8"),
                            encoding="utf-8")
        at = wait_log(sim, "scene.counter", "v2", at)
        at = wait_log(sim, "scene", "counter: running", at)
        click(sim, "plus")
        wait_log(sim, "scene.counter", "count=2", at)
        stop_child(sim)
    finally:
        sim.kill()


def case_scene_error(sim_path, tmp_root):
    """A scene that fails while building its tree: the error (file and line)
    is logged, the back gesture closes it to the self-check screen, and the
    screenshot taken at the end is not blank."""
    data_dir = tmp_root / "scene-error"
    shot = data_dir / "shot.png"
    data_dir.mkdir()
    result = run_sim(sim_path, data_dir, [
        "--scenes-dir", str(SCENE_FIXTURES), "--open-scene", "broken",
        "--swipe", "4,240,240,240@1000", "--screenshot", str(shot)])
    expect(result.returncode == 0, f"exit code {result.returncode}, stderr:\n{result.stderr}")
    lines = result.stdout.splitlines()

    def index(tag, needle):
        for i, line in enumerate(lines):
            m = LOG_LINE.match(line)
            if m and m.group(1) == tag and needle in m.group(2):
                return i
        raise SmokeError(f"no log line '{tag}: ...{needle}...':\n{result.stdout}")

    failed = index("scene.broken", "main.lua:5:")
    expect(index("scene", "broken: error") > failed, "the error was not published")
    expect(index("scene", "broken: closed") > failed,
           f"the back gesture did not close the scene:\n{result.stdout}")
    check_screenshot(shot)


def read_until(stream, needle, timeout_s, sink):
    """Reads `stream` (a binary pipe) byte by byte into `sink` until `needle`
    has appeared, on a thread; returns that thread's Event."""
    seen = threading.Event()

    def run():
        buf = b""
        while True:
            b = stream.read(1)
            if not b:
                break
            buf += b
            if needle.encode() in buf:
                seen.set()
        sink.append(buf.decode("utf-8", "replace"))
        seen.set()

    threading.Thread(target=run, daemon=True).start()
    return seen


# The confirm dialog's "Yes" button for a scene delete, in English on the
# default 480x480 rounded_rect screen: SystemLayer::open_confirm() in
# components/wisp_ui/src/system_layer.cpp. Move it when that layout or the
# delete prompt changes.
SCENE_DELETE_YES_TAP = RESET_YES_TAP
SCENE_DELETE_LOG = "counter: delete waiting for confirmation on the screen"


def case_scene_push(sim_path, tmp_root):
    """tools/wisp_scene.py pairs (the code from "debug pair-code" typed into
    its stdin), pushes the counter example and opens it; deleting it with
    its data then needs a tap on "Yes" on the device, after which no scene
    is left."""
    data_dir = tmp_root / "scene-push"
    config_dir = tmp_root / "scene-push-config"
    data_dir.mkdir()
    (data_dir / "settings.json").write_text(json.dumps({"sys": {"lang": "en"}}))
    port = free_port()
    sim = scene_sim(sim_path, data_dir, [], http_port=port)
    tool = ws = None
    try:
        expect(sim.wait_line(lambda l: l.startswith("WISP_HTTP "), TIMEOUT_S) is not None,
               f"no WISP_HTTP line:\n{sim.tail()}")
        env = dict(os.environ, WISP_SCENE_TIMEOUT_S=str(TIMEOUT_S))
        tool = subprocess.Popen(
            [sys.executable, "-I", str(WISP_SCENE), "--host", f"127.0.0.1:{port}",
             "--config-dir", str(config_dir), "push", str(EXAMPLES / "counter"), "--open"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        tool_err = []
        asked = read_until(tool.stderr, "配对码", TIMEOUT_S, tool_err)
        expect(asked.wait(TIMEOUT_S), f"wisp_scene.py never asked for the code: {tool_err}")
        mark = len(sim.lines)
        sim.send("debug pair-code")
        line = sim.wait_line(lambda l: l.startswith("pair-code="), TIMEOUT_S, start=mark)
        expect(line is not None and re.fullmatch(r"pair-code=\d{6}", line),
               f"'debug pair-code' printed {line!r}")
        try:
            out, _ = tool.communicate((line[len("pair-code="):] + "\n").encode(),
                                      timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            raise SmokeError("wisp_scene.py push did not finish") from exc
        out = out.decode("utf-8", "replace")
        expect(tool.returncode == 0 and "running" in out,
               f"wisp_scene.py push -> {tool.returncode}:\n{out}\n{''.join(tool_err)}")
        scene_dir = data_dir / "int" / "scenes" / "counter"
        expect(scene_dir.is_dir(), f"{scene_dir}: not installed there")

        token = json.loads((config_dir / "scene_tool.json").read_text())[
            "hosts"][f"127.0.0.1:{port}"]["token"]
        cookie = f"wisp_token={token}"
        ws = WsClient(port, cookie)
        ws.sock.settimeout(TIMEOUT_S)
        ws.send_text(json.dumps({"sub": ["events", "log"]}))
        while True:
            text = ws.recv_text()
            expect(text is not None, "the device closed the socket before replaying its log")
            if json.loads(text).get("t") == "log":
                break

        status, body = api_request(port, "DELETE", "/api/v1/scenes/counter?data=1",
                                   {"Cookie": cookie, "X-Wisp": "1"})
        expect(status == 202, f"DELETE /api/v1/scenes/counter?data=1 -> {status} {body}")
        deadline = time.monotonic() + TIMEOUT_S
        tapped = False
        states = []
        while "deleted" not in states:
            expect(time.monotonic() < deadline,
                   f"not deleted within {TIMEOUT_S}s (tapped: {tapped}); states {states}")
            if not select.select([ws.sock], [], [], 0.5)[0]:
                continue
            text = ws.recv_text()
            expect(text is not None, f"the device closed the socket; states {states}")
            msg = json.loads(text)
            if msg.get("t") == "event" and msg.get("topic") == "scene.state":
                states.append(msg["payload"].get("state"))
            elif (msg.get("t") == "log" and not tapped
                  and SCENE_DELETE_LOG in msg.get("msg", "")):
                sim.send("tap {},{}".format(*SCENE_DELETE_YES_TAP))
                tapped = True
        status, body = api_request(port, "GET", "/api/v1/scenes", {"Cookie": cookie})
        expect(status == 200 and json.loads(body) == {"scenes": []},
               f"GET /api/v1/scenes after the delete -> {status} {body}")
        expect(not (data_dir / "int" / "scenes" / "counter").exists(),
               "the scene's directory is still there")
        stop_child(sim)
    finally:
        if ws is not None:
            ws.close()
        if tool is not None and tool.poll() is None:
            tool.kill()
            tool.wait()
        sim.kill()


CASES = (
    ("expected_manifest_ids_unit", case_expected_manifest_ids_unit),
    ("default", case_default),
    ("disable_imu", case_disable_imu),
    ("no_sd", case_no_sd),
    ("shape_circle", case_shape_circle),
    ("disable_wifi", case_disable_wifi),
    ("http", case_http),
    ("factory_reset", case_factory_reset),
    ("firmware", case_firmware),
    ("console", case_console),
    ("scene_counter", case_scene_counter),
    ("scene_hot_reload", case_scene_hot_reload),
    ("scene_error", case_scene_error),
    ("scene_push", case_scene_push),
)


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <path-to-wisp_sim>", file=sys.stderr)
        return 2
    sim_path = pathlib.Path(argv[1]).resolve()
    if not sim_path.is_file():
        print(f"{sim_path}: not found", file=sys.stderr)
        return 2

    tmp_root = pathlib.Path(tempfile.mkdtemp(prefix="wisp_sim_smoke_"))
    try:
        for name, case in CASES:
            try:
                case(sim_path, tmp_root)
            except Exception as exc:  # noqa: BLE001 -- any case failure (an
                # assertion from expect()/SmokeError, or a plain bug like a
                # KeyError/json decode error) should produce a readable FAIL
                # line plus the traceback, not an unhandled crash.
                print(f"FAIL {name}: {exc}", file=sys.stderr)
                print(traceback.format_exc(), file=sys.stderr)
                return 1
            print(f"PASS {name}")
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
