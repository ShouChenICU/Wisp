#!/usr/bin/env python3
"""wisp_scene: push scenes to a Wisp device and watch them run
(scene-runtime-core spec 7.3). Standard library only.

  wisp_scene.py push <dir> [--open] [--watch]   pack <dir> and install it
  wisp_scene.py ls                              list the installed scenes
  wisp_scene.py logs <id>                       follow one scene's log

Global options: --host (default wisp.local:8900) and --config-dir (default
$XDG_CONFIG_HOME/wisp, else ~/.config/wisp).

Everything goes through the paired admin API under /api/v1 (connectivity
spec 6.1, 6.3). The first call to a host pairs: POST /pair/start puts a
code on the device's screen, the user types it here, and POST
/pair/confirm answers with the wisp_token cookie. The token is kept in
<config-dir>/scene_tool.json, keyed by host, mode 0600, replaced in one
step (written aside, then os.replace()). A 401 means the device forgot
the token (revoked, factory reset): pair again once and retry. The token
is never printed, logged or put in an error message. The admin server
speaks plain HTTP (connectivity spec 6), so the token crosses the network
in the clear: use this on the local network only.

push packs only what a package may hold -- scene.json, the entry file,
lib/**/*.lua and assets/** -- with ZIP_DEFLATED, lists everything else as
skipped (data/ silently: it is the device's store), and refuses a JPEG
under assets/ that is not baseline, since the device cannot decode it
(scene-runtime-core spec 4.3). --watch compares a snapshot of the files
every 0.5 s and pushes again once a change has held still for 300 ms;
the device reloads an installed scene that is running.

logs subscribes to the log over the admin WebSocket ({"sub":["log"]})
and prints only the lines tagged scene.<id>.
"""

import argparse
import base64
import hashlib
import http.client
import io
import json
import os
import pathlib
import re
import socket
import struct
import sys
import tempfile
import time
import zipfile
import zlib

DEFAULT_HOST = "wisp.local:8900"
DEFAULT_PORT = 8900
API = "/api/v1"
TOKEN_FILE = "scene_tool.json"
# Connecting and each HTTP request; the log stream, once open, waits as
# long as the device stays quiet. WISP_SCENE_TIMEOUT_S overrides it (the
# tests shorten it).
TIMEOUT_S = float(os.environ.get("WISP_SCENE_TIMEOUT_S") or 30)
# The device refuses a package over 512 KiB (spec 6.2); say so before
# uploading.
MAX_CODE_BYTES = 512 * 1024
WATCH_PERIOD_S = 0.5
DEBOUNCE_S = 0.3
SCENE_ID = re.compile(r"[a-z0-9_-]{1,32}")


class ToolError(Exception):
    """A failure to report in one line, without a traceback."""


def default_config_dir():
    base = os.environ.get("XDG_CONFIG_HOME")
    root = pathlib.Path(base) if base else pathlib.Path.home() / ".config"
    return root / "wisp"


# --- packing ---------------------------------------------------------------

def jpeg_kind(data):
    """'baseline', 'unsupported' (progressive, lossless, arithmetic) or
    'not_jpeg', from the first frame header; mirrors the device's reader
    (components/wisp_port_esp/include/wisp/port/esp/jpeg_header.hpp)."""
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        return "not_jpeg"
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            return "not_jpeg"
        m = data[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m == 0x01 or 0xD0 <= m <= 0xD8:
            i += 2
            continue
        if m in (0xD9, 0xDA):
            return "not_jpeg"
        size = data[i + 2] << 8 | data[i + 3]
        if size < 2 or size > len(data) - i - 2:
            return "not_jpeg"
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            if size < 7:
                return "not_jpeg"
            height = data[i + 5] << 8 | data[i + 6]
            width = data[i + 7] << 8 | data[i + 8]
            ok = m in (0xC0, 0xC1) and width > 0 and height > 0
            return "baseline" if ok else "unsupported"
        i += 2 + size
    return "not_jpeg"


def reserved(name):
    # The device's write bookkeeping names (StorageManager); never package
    # files.
    return name.endswith(".tmp") or name.endswith(".new")


def read_manifest(directory):
    path = directory / "scene.json"
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ToolError(f"{directory}: no scene.json") from None
    except (OSError, ValueError) as exc:
        raise ToolError(f"scene.json: {exc}") from None
    if not isinstance(manifest, dict) or not isinstance(manifest.get("id"), str):
        raise ToolError("scene.json: needs an \"id\" string")
    entry = manifest.get("entry", "main.lua")
    if not isinstance(entry, str):
        raise ToolError("scene.json: \"entry\" must be a string")
    return manifest, entry


def walk_package(directory, entry):
    """([(relative path, Path)] a package may hold, [skipped]), sorted. A
    skipped directory is listed once, with a trailing '/'. Symlinks are
    never followed, file or directory: a link inside a scene can point
    anywhere (a cloned scene's assets/x.png -> ~/.ssh/id_ed25519), and what
    is packed ends up on the device and in its exports."""
    directory = pathlib.Path(directory)
    files, skipped = [], []
    for current, dirs, names in os.walk(directory):
        rel_dir = pathlib.Path(current).relative_to(directory).as_posix()
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        keep = []
        for d in sorted(dirs):
            rel = rel_dir + d
            if not rel_dir and d == "data" and not os.path.islink(os.path.join(current, d)):
                continue  # the device's store; never part of a package
            if ((rel_dir or d in ("lib", "assets")) and not d.startswith(".")
                    and not reserved(d) and not os.path.islink(os.path.join(current, d))):
                keep.append(d)
            else:
                skipped.append(rel + "/")
        dirs[:] = keep
        for name in sorted(names):
            rel = rel_dir + name
            path = pathlib.Path(current) / name
            hidden = name.startswith(".") or reserved(name) or path.is_symlink()
            if not hidden and (rel in ("scene.json", entry)
                               or (rel.startswith("lib/") and name.endswith(".lua")
                                   and len(name) > 4)
                               or rel.startswith("assets/")):
                files.append((rel, path))
            else:
                skipped.append(rel)
    return sorted(files), sorted(skipped)


def collect(directory):
    """(manifest, [(relative path, Path)] to pack, [skipped])."""
    manifest, entry = read_manifest(pathlib.Path(directory))
    files, skipped = walk_package(directory, entry)
    if entry not in [rel for rel, _ in files]:
        raise ToolError(f"{entry}: the entry file is missing")
    return manifest, files, skipped


def pack(directory):
    """(zip bytes, manifest, skipped) for the scene in `directory`."""
    manifest, files, skipped = collect(directory)
    total = 0
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, path in files:
            data = path.read_bytes()
            total += len(data)
            if rel.lower().endswith((".jpg", ".jpeg")) and jpeg_kind(data) != "baseline":
                raise ToolError(f"{rel}: not a baseline JPEG (progressive JPEGs cannot be "
                                "shown on the device); save it as baseline")
            z.writestr(zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0)), data,
                       compress_type=zipfile.ZIP_DEFLATED)
    if total > MAX_CODE_BYTES:
        raise ToolError(f"the package is {total} bytes; a scene holds at most {MAX_CODE_BYTES}")
    return buf.getvalue(), manifest, skipped


def snapshot(directory):
    """{relative path: (size, crc32)} of the files push would pack, for
    --watch: editing a README or anything else outside the package pushes
    nothing. Works on a scene whose scene.json is broken (the entry is then
    taken to be main.lua), so fixing it is a change too."""
    try:
        _, entry = read_manifest(pathlib.Path(directory))
    except ToolError:
        entry = "main.lua"
    out = {}
    for rel, path in walk_package(directory, entry)[0]:
        try:
            data = path.read_bytes()
        except OSError:
            data = None
        out[rel] = (-1, 0) if data is None else (len(data), zlib.crc32(data))
    try:
        data = (pathlib.Path(directory) / "scene.json").read_bytes()
        out["scene.json"] = (len(data), zlib.crc32(data))
    except OSError:
        pass
    return out


# --- token store -----------------------------------------------------------

class TokenStore:
    """<config-dir>/scene_tool.json: {"hosts": {"<host>": {"token": ...}}}."""

    def __init__(self, config_dir):
        self.dir = pathlib.Path(config_dir)
        self.path = self.dir / TOKEN_FILE

    def _load(self):
        """The file's contents; empty when it does not exist. A file that
        cannot be read or parsed is an error rather than empty, so put()
        never overwrites the other hosts' tokens with nothing. A file others
        may read is narrowed to 0600 first."""
        try:
            st = self.path.stat()
        except FileNotFoundError:
            return {"hosts": {}}
        except OSError as exc:
            raise ToolError(f"{self.path}: {exc.strerror}") from None
        try:
            if st.st_mode & 0o077:
                os.chmod(self.path, 0o600)
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ToolError(f"{self.path}: unreadable ({type(exc).__name__}); "
                            "fix or delete it to pair again") from None
        if not isinstance(data, dict) or not isinstance(data.get("hosts"), dict):
            raise ToolError(f"{self.path}: not a token file; fix or delete it to pair again")
        return data

    def get(self, host):
        entry = self._load()["hosts"].get(host)
        token = entry.get("token") if isinstance(entry, dict) else None
        return token if isinstance(token, str) and token else None

    def put(self, host, token):
        data = self._load()
        if token is None:
            data["hosts"].pop(host, None)
        else:
            data["hosts"][host] = {"token": token}
        self._save(data)

    def drop(self, host):
        self.put(host, None)

    def _save(self, data):
        self.dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        # mkstemp() makes the file 0600 from the start: the token is never
        # readable by others, not even for a moment.
        fd, tmp = tempfile.mkstemp(dir=self.dir, prefix=".scene_tool.", suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=1, sort_keys=True)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(tmp, 0o600)
            os.replace(tmp, self.path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise


# --- the admin API ---------------------------------------------------------

def parse_json(body):
    """A 2xx body as a JSON object; ToolError for anything else."""
    try:
        value = json.loads(body)
    except ValueError:
        value = None
    if not isinstance(value, dict):
        raise ToolError("the device sent a reply this tool does not understand")
    return value


def error_text(status, body):
    """One line from an API error: {"error": {"code", "message"[, "file",
    "line"]}}; a compile error reads "<file>:<line>: <message>"."""
    try:
        err = json.loads(body).get("error")
    except (ValueError, AttributeError):
        err = None
    if not isinstance(err, dict):
        return f"HTTP {status}"
    code = err.get("code") or f"HTTP {status}"
    message = str(err.get("message") or "")
    where = ""
    if err.get("file") and err.get("line") is not None:
        where = f"{err['file']}:{err['line']}: "
        prefix = f"{err['file']}:{err['line']}:"
        if message.startswith(prefix):
            message = message[len(prefix):].lstrip()
    if err.get("retry_after_s"):
        message += f" (retry in {err['retry_after_s']} s)"
    return f"{where}{message} [{code}]"


class Client:
    def __init__(self, host, store, ask_code, out=sys.stdout, err=sys.stderr):
        self.host = host
        name, sep, port = host.rpartition(":")
        if sep and port.isdigit():
            self.hostname, self.port = name, int(port)
        else:
            self.hostname, self.port = host, DEFAULT_PORT
        self.store = store
        self.ask_code = ask_code
        self.out = out
        self.err = err
        self.token = store.get(host)

    def _headers(self, auth):
        h = {"X-Wisp": "1"}
        if auth and self.token:
            h["Cookie"] = f"wisp_token={self.token}"
        return h

    def request(self, method, path, body=None, content_type=None, auth=True):
        headers = self._headers(auth)
        if content_type:
            headers["Content-Type"] = content_type
        conn = http.client.HTTPConnection(self.hostname, self.port, timeout=TIMEOUT_S)
        try:
            conn.request(method, API + path, body=body, headers=headers)
            res = conn.getresponse()
            return res.status, res, res.read()
        except OSError as exc:
            raise ToolError(f"cannot reach {self.host}: {exc}") from None
        finally:
            conn.close()

    def pair(self):
        status, _, body = self.request("POST", "/pair/start", b"{}", "application/json",
                                       auth=False)
        if status != 202:
            raise ToolError(f"pairing: {error_text(status, body)}")
        code = self.ask_code().strip()
        if not code:
            raise ToolError("pairing: no code given")
        status, res, body = self.request("POST", "/pair/confirm",
                                         json.dumps({"code": code}).encode(),
                                         "application/json", auth=False)
        if status != 200:
            raise ToolError(f"pairing: {error_text(status, body)}")
        token = None
        for value in res.headers.get_all("Set-Cookie") or []:
            m = re.match(r"\s*wisp_token=([^;\s]+)", value)
            if m:
                token = m.group(1)
        if not token:
            raise ToolError("pairing: the device sent no token")
        self.token = token
        self.store.put(self.host, token)
        print(f"已与 {self.host} 配对", file=self.err)

    def call(self, method, path, body=None, content_type=None):
        """(status, body); pairs first if needed, and once more on 401."""
        if not self.token:
            self.pair()
        status, _, data = self.request(method, path, body, content_type)
        if status == 401:
            self.token = None
            self.store.drop(self.host)
            self.pair()
            status, _, data = self.request(method, path, body, content_type)
        return status, data

    def json_call(self, method, path, body=None, content_type=None, want=(200,)):
        status, data = self.call(method, path, body, content_type)
        if status not in want:
            raise ToolError(error_text(status, data))
        return parse_json(data) if data else {}

    # A WebSocket: the upgrade, masked text frames out, text frames in.
    def ws_connect(self):
        if not self.token:
            self.pair()
        for attempt in range(2):
            status, ws = self._ws_upgrade()
            if ws is not None:
                return ws
            if status == 401 and attempt == 0:
                self.token = None
                self.store.drop(self.host)
                self.pair()
                continue
            raise ToolError(f"log stream refused: HTTP {status}")
        raise ToolError("log stream refused")

    def _ws_upgrade(self):
        """(status, WebSocket or None)."""
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f"GET {API}/ws HTTP/1.1\r\nHost: {self.host}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n")
        for k, v in self._headers(True).items():
            request += f"{k}: {v}\r\n"
        try:
            sock = socket.create_connection((self.hostname, self.port), timeout=TIMEOUT_S)
        except OSError as exc:
            raise ToolError(f"cannot reach {self.host}: {exc}") from None
        try:
            sock.sendall((request + "\r\n").encode())
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = sock.recv(1)
                if not chunk:
                    break
                head += chunk
            lines = head.decode("latin-1").split("\r\n")
            parts = lines[0].split()
            status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
            if status != 101:
                sock.close()
                return status, None
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC11B9A").encode()).digest()).decode()
            got = [l.split(":", 1)[1].strip() for l in lines[1:]
                   if l.lower().startswith("sec-websocket-accept:")]
            if got != [accept]:
                raise ToolError("log stream: the device answered the upgrade wrongly")
            # The device sends no keepalives: a quiet scene means a quiet
            # socket, for as long as it likes.
            sock.settimeout(None)
            return status, WebSocket(sock)
        except OSError as exc:
            sock.close()
            raise ToolError(f"log stream from {self.host}: {exc}") from None
        except BaseException:
            sock.close()
            raise


class WebSocket:
    def __init__(self, sock):
        self.sock = sock

    def _read(self, n):
        """Exactly n bytes; ToolError if the connection ends first."""
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ToolError("the device dropped the log stream")
            data += chunk
        return data

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        if len(payload) < 126:
            head = bytes([0x81, 0x80 | len(payload)])
        else:
            head = bytes([0x81, 0x80 | 126]) + struct.pack(">H", len(payload))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + masked)

    def recv_text(self):
        """The next text message, its fragments joined; None once the device
        closes the stream with a close frame. A connection that just ends is
        a ToolError, like every socket error."""
        parts = []
        while True:
            head = self._read(2)
            fin, opcode, size = head[0] & 0x80, head[0] & 0x0F, head[1] & 0x7F
            if size == 126:
                size = struct.unpack(">H", self._read(2))[0]
            elif size == 127:
                size = struct.unpack(">Q", self._read(8))[0]
            body = self._read(size) if size else b""
            if opcode == 0x8:
                return None
            if opcode == 0x9:  # ping
                self.sock.sendall(bytes([0x8A, 0x80 | len(body)]) + b"\0\0\0\0" + body)
                continue
            if opcode == 0x1 or (opcode == 0x0 and parts):
                parts.append(body)
                if fin:
                    return b"".join(parts).decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --- commands --------------------------------------------------------------

def check_id(scene_id):
    if not SCENE_ID.fullmatch(scene_id):
        raise ToolError(f"invalid scene id: {scene_id!r}")
    return scene_id


def push_once(client, directory, open_after, out):
    blob, manifest, skipped = pack(directory)
    for rel in skipped:
        print(f"已跳过 {rel}", file=out)
    status, data = client.call("POST", "/scenes", blob, "application/zip")
    if status != 201:
        raise ToolError(error_text(status, data))
    item = parse_json(data)
    hosts = ", ".join(item.get("hosts") or []) or "-"
    print(f"{item.get('id')} {item.get('version')} 域名: {hosts} 状态: {item.get('state')}",
          file=out)
    if open_after:
        scene_id = check_id(item.get("id", ""))
        state = client.json_call("POST", f"/scenes/{scene_id}/open", b"{}",
                                 "application/json").get("state")
        print(f"{scene_id} 已打开 状态: {state}", file=out)


def wait_for_change(directory, before):
    """Blocks until the files differ from `before` and then hold still for
    DEBOUNCE_S; returns the new snapshot."""
    while True:
        time.sleep(WATCH_PERIOD_S)
        now = snapshot(directory)
        if now == before:
            continue
        while True:
            time.sleep(DEBOUNCE_S)
            settled = snapshot(directory)
            if settled == now:
                return settled
            now = settled


def cmd_push(client, args, out, err):
    directory = pathlib.Path(args.dir)
    seen = snapshot(directory)
    try:
        push_once(client, directory, args.open, out)
    except ToolError as exc:
        if not args.watch:
            raise
        print(f"error: {exc}", file=err)
    if not args.watch:
        return 0
    print(f"正在监视 {directory}（Ctrl-C 结束）", file=err)
    while True:
        seen = wait_for_change(directory, seen)
        try:
            push_once(client, directory, False, out)
        except ToolError as exc:
            print(f"error: {exc}", file=err)
        out.flush()


def cmd_ls(client, args, out, err):
    scenes = client.json_call("GET", "/scenes").get("scenes", [])
    if not isinstance(scenes, list) or not all(isinstance(x, dict) for x in scenes):
        raise ToolError("the device sent a reply this tool does not understand")
    if not scenes:
        print("还没有安装场景", file=out)
        return 0
    for s in scenes:
        hosts = ",".join(s.get("hosts") or []) or "-"
        line = (f"{s.get('id')}\t{s.get('name')}\t{s.get('version')}\t{s.get('state')}\t"
                f"code={s.get('code_bytes')}\tdata={s.get('data_bytes')}\thosts={hosts}")
        if s.get("error"):
            line += f"\terror={s['error']}"
        print(line, file=out)
    return 0


def cmd_logs(client, args, out, err):
    tag = "scene." + check_id(args.id)
    ws = client.ws_connect()
    try:
        ws.send_text(json.dumps({"sub": ["log"]}))
        while True:
            text = ws.recv_text()
            if text is None:
                print("the device closed the log stream", file=err)
                return 0
            try:
                msg = json.loads(text)
            except ValueError:
                continue
            if not isinstance(msg, dict):
                continue
            if msg.get("t") == "log" and msg.get("tag") == tag:
                print(f"{msg.get('level', '?')} {msg.get('msg', '')}", file=out)
                out.flush()
            elif msg.get("t") == "dropped":
                print("(some log lines were dropped)", file=err)
    except OSError as exc:
        raise ToolError(f"log stream from {client.host}: {exc}") from None
    finally:
        ws.close()


def read_code_from_stdin(err):
    def ask():
        print("请输入设备屏幕上的配对码: ", end="", file=err, flush=True)
        line = sys.stdin.readline()
        return line
    return ask


def main(argv=None):
    parser = argparse.ArgumentParser(prog="wisp_scene.py", description=__doc__.split("\n")[0])
    parser.add_argument("--host", default=DEFAULT_HOST, help="device host[:port]")
    parser.add_argument("--config-dir", default=None,
                        help="where the pairing token is kept "
                             "(default: $XDG_CONFIG_HOME/wisp, else ~/.config/wisp)")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("push", help="pack a scene directory and install it")
    p.add_argument("dir")
    p.add_argument("--open", action="store_true", help="open it after installing")
    p.add_argument("--watch", action="store_true", help="push again after every save")
    sub.add_parser("ls", help="list the installed scenes")
    p = sub.add_parser("logs", help="follow one scene's log")
    p.add_argument("id")
    args = parser.parse_args(argv)

    out, err = sys.stdout, sys.stderr
    config_dir = pathlib.Path(args.config_dir) if args.config_dir else default_config_dir()
    try:
        if args.command == "logs":
            check_id(args.id)
        client = Client(args.host, TokenStore(config_dir), read_code_from_stdin(err), out, err)
        handler = {"push": cmd_push, "ls": cmd_ls, "logs": cmd_logs}[args.command]
        return handler(client, args, out, err)
    except ToolError as exc:
        print(f"error: {exc}", file=err)
        return 1
    except OSError as exc:
        # Anything the commands did not turn into a ToolError themselves.
        print(f"error: {args.host}: {exc}", file=err)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
