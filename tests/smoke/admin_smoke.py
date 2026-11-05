#!/usr/bin/env python3
"""End-to-end smoke test of the simulator's admin API (connectivity spec 6).

Starts tools/fake_openai.py and one wisp_sim (--headless, a fresh
temporary data dir) and then, over loopback:

  1. reads the admin port from the WISP_HTTP line (prefix + single-line
     JSON, like every machine-readable line);
  2. pairs like a browser: POST /pair/start, the code read back with the
     simulator-only console command "debug pair-code" written to the
     simulator's stdin, POST /pair/confirm for the wisp_token cookie;
  3. reads /device, /devices and /wifi, and round-trips a setting;
  4. model profiles (/ai/*): creates an llm profile on the fake endpoint
     and tests it, once with a wrong key (http 401) and once with
     "test-key" (both models); creates an asr profile that reuses that key
     through api_key_from and tests it; checks that activating a second
     llm profile leaves the asr one active, that the active profile of a
     kind with others cannot be deleted (409 active_profile), that a
     platform without /models reports no_model_list and that the old /llm
     routes are gone;
  5. scenes (scene-runtime-core spec 7.1): uploads scenes/examples/counter
     as packed by tools/wisp_scene.py, lists, opens, reloads and closes it,
     exports it with and without its data, and has a package with a syntax
     error refused with its file and line;
  6. subscribes to the log over the WebSocket and waits for an entry;
  7. checks the CSRF guard (no X-Wisp: 403), the Host check (a foreign
     Host: 403) and the pairing lock (wrong codes until 429 with
     Retry-After), after which the paired cookie still works;
  8. stops the simulator with SIGTERM, which must end it cleanly (exit 0).

The API key may appear in no HTTP response, WebSocket message or
simulator output; the pairing token only in the one Set-Cookie that
delivered it; the pairing code only on the "debug pair-code" line.

--http-port 0 disables the simulator's admin server, so the port is picked
here (bind to port 0, read it, close) and passed in; if the simulator then
loses the race for it ("admin server failed to start"), the run is retried
on a new port. fake_openai.py binds port 0 itself and reports its port.

Both child processes are killed on any failure. In case this script itself
is killed, wisp_sim gets --exit-after-ms as a backstop and fake_openai.py
runs with --exit-on-eof on a stdin pipe that dies with this process.

Registered with ctest as "admin_smoke" (tests/CMakeLists.txt).

Usage: python3 tests/smoke/admin_smoke.py <path-to-wisp_sim>
"""

import http.client
import importlib.util
import io
import json
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import zipfile

from sim_smoke import Child, SmokeError, WsClient, click, expect, free_port

ROOT = pathlib.Path(__file__).resolve().parents[2]
FAKE_OPENAI = ROOT / "tools" / "fake_openai.py"
WISP_SCENE = ROOT / "tools" / "wisp_scene.py"
COUNTER = ROOT / "scenes" / "examples" / "counter"
API_KEY = "test-key"
STEP_TIMEOUT_S = 20
WS_LOG_TIMEOUT_S = 30
# Long enough for every step; only a killed run ever reaches it.
BACKSTOP_MS = 120000
PORT_ATTEMPTS = 3
HTTP_FAILED_LOG = "admin server failed to start"


class Api:
    """Requests to the admin API; every response body and header is kept for
    the secret scan at the end."""

    def __init__(self, port):
        self.port = port
        self.cookie = None
        self.seen = []

    def call(self, method, path, body=None, headers=None, csrf=True, auth=True, raw=None,
             binary=False):
        """raw: bytes sent as they are (with their own Content-Type in
        `headers`); binary: the body comes back as bytes."""
        h = dict(headers or {})
        if csrf and method != "GET":
            h["X-Wisp"] = "1"
        if auth and self.cookie:
            h["Cookie"] = self.cookie
        data = raw
        if body is not None:
            data = json.dumps(body)
            h["Content-Type"] = "application/json"
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=STEP_TIMEOUT_S)
        try:
            conn.request(method, "/api/v1" + path, body=data, headers=h)
            res = conn.getresponse()
            content = res.read()
            res_headers = {k.lower(): v for k, v in res.getheaders()}
        finally:
            conn.close()
        text = content.decode("utf-8", "replace" if binary else "strict")
        self.seen.append(f"{method} {path} -> {res.status} {res_headers} {text}")
        return res.status, res_headers, content if binary else text

    def ok_json(self, method, path, body=None, status=200):
        got, _, text = self.call(method, path, body)
        expect(got == status, f"{method} {path} -> {got} {text}")
        return json.loads(text) if text else None


def start_sim(sim_path, data_dir):
    """Starts wisp_sim on a free port; returns (child, port)."""
    for _ in range(PORT_ATTEMPTS):
        port = free_port()
        sim = Child([str(sim_path), "--headless", "--data-dir", str(data_dir),
                     "--http-port", str(port), "--exit-after-ms", str(BACKSTOP_MS)], stdin=True)
        try:
            line = sim.wait_line(
                lambda l: l.startswith("WISP_HTTP ") or HTTP_FAILED_LOG in l, STEP_TIMEOUT_S)
            expect(line is not None, f"no WISP_HTTP line from wisp_sim:\n{sim.tail()}")
            if line.startswith("WISP_HTTP "):
                announced = json.loads(line[len("WISP_HTTP "):])
                expect(announced == {"port": port},
                       f"WISP_HTTP == {announced!r}, asked for {port}")
                return sim, port
        except BaseException:
            sim.kill()
            raise
        sim.kill()
    raise SmokeError(f"the admin server failed to bind {PORT_ATTEMPTS} free ports in a row")


def pair(api, sim):
    info = api.ok_json("GET", "/info")
    expect(info.get("paired") is False, f"/info before pairing: {info}")
    api.ok_json("POST", "/pair/start", {}, status=202)

    mark = len(sim.lines)
    sim.send("debug pair-code")
    line = sim.wait_line(lambda l: l.startswith("pair-code="), STEP_TIMEOUT_S, start=mark)
    expect(line is not None, f"no answer to 'debug pair-code':\n{sim.tail()}")
    code = line[len("pair-code="):]
    expect(re.fullmatch(r"\d{6}", code), f"'debug pair-code' printed {line!r}")

    status, headers, text = api.call("POST", "/pair/confirm", {"code": code})
    expect(status == 200 and json.loads(text) == {"paired": True},
           f"POST /pair/confirm -> {status} {text}")
    cookie = headers.get("set-cookie", "").split(";")[0]
    expect(re.fullmatch(r"wisp_token=[A-Za-z0-9_-]{20,}", cookie), f"Set-Cookie: {cookie!r}")
    api.cookie = cookie
    info = api.ok_json("GET", "/info")
    expect(info.get("paired") is True, f"/info after pairing: {info}")
    return code, cookie[len("wisp_token="):]


def check_routes(api):
    device = api.ok_json("GET", "/device")
    expect(device.get("boot", {}).get("notes", {}).get("net") == "off", f"/device: {device}")
    devices = api.ok_json("GET", "/devices")
    expect("net.wifi" in json.dumps(devices), f"/devices lacks net.wifi: {devices}")
    wifi = api.ok_json("GET", "/wifi")
    expect(isinstance(wifi, dict) and wifi, f"/wifi: {wifi}")

    api.ok_json("PUT", "/settings", {"brightness": 120})
    settings = api.ok_json("GET", "/settings")
    expect(settings.get("brightness") == 120, f"/settings after PUT: {settings}")
    expect(settings.get("ntp_servers") == "pool.ntp.org,ntp.aliyun.com",
           f"/settings ntp_servers: {settings}")

    # No Wi-Fi saved: nothing synced, and a sync is refused.
    time = api.ok_json("GET", "/system/time")
    expect(time == {"synced": False, "last_sync": None, "server": "", "time": None},
           f"/system/time: {time}")
    status, _, text = api.call("POST", "/system/time/sync", {})
    expect(status == 409 and json.loads(text)["error"]["code"] == "no_network",
           f"POST /system/time/sync offline -> {status} {text}")


def check_models(api, llm_port):
    base_url = f"http://127.0.0.1:{llm_port}/v1"
    presets = [p["id"] for p in api.ok_json("GET", "/ai/presets")["presets"]]
    expect(presets[:2] == ["custom", "openai"] and len(presets) == 13, f"/ai/presets: {presets}")

    chat = api.ok_json("POST", "/ai/profiles", {
        "kind": "llm", "name": "Chat", "preset": "custom", "base_url": base_url,
        "api_key": "wrong-key"}, status=201)
    expect(chat.get("base_url") == base_url and chat.get("has_key") is True
           and "api_key" not in chat, f"POST /ai/profiles: {chat}")
    probe = api.ok_json("POST", f"/ai/profiles/{chat['id']}/test", {})
    expect(probe.get("ok") is False and probe.get("error") == "http"
           and probe.get("http_status") == 401, f"test with a wrong key: {probe}")
    api.ok_json("PUT", f"/ai/profiles/{chat['id']}", {"api_key": API_KEY})
    probe = api.ok_json("POST", f"/ai/profiles/{chat['id']}/test", {})
    expect(probe.get("ok") is True and probe.get("models") == ["fake-1", "fake-2"],
           f"test: {probe}")
    chat = api.ok_json("PUT", f"/ai/profiles/{chat['id']}", {"model": "fake-2"})
    expect(chat.get("model") == "fake-2" and chat.get("has_key") is True, f"PUT: {chat}")

    # The key reaches a same-origin asr profile without passing through
    # this client.
    asr = api.ok_json("POST", "/ai/profiles", {
        "kind": "asr", "name": "Whisper", "preset": "custom", "base_url": base_url,
        "api_key_from": chat["id"]}, status=201)
    expect(asr.get("has_key") is True, f"asr with api_key_from: {asr}")
    probe = api.ok_json("POST", f"/ai/profiles/{asr['id']}/test", {})
    expect(probe.get("ok") is True, f"asr test: {probe}")

    # Per-kind activation.
    second = api.ok_json("POST", "/ai/profiles", {
        "kind": "llm", "name": "No list", "preset": "custom",
        "base_url": f"http://127.0.0.1:{llm_port}/nolist"}, status=201)
    state = api.ok_json("GET", "/ai/profiles")
    expect(state["active"] == {"llm": chat["id"], "asr": asr["id"], "tts": ""}, f"{state}")
    state = api.ok_json("POST", "/ai/active", {"id": second["id"]})
    expect(state["active"] == {"llm": second["id"], "asr": asr["id"], "tts": ""}, f"{state}")
    status, _, text = api.call("DELETE", f"/ai/profiles/{second['id']}")
    expect(status == 409 and '"active_profile"' in text, f"DELETE active -> {status} {text}")
    probe = api.ok_json("POST", f"/ai/profiles/{second['id']}/test", {})
    expect(probe.get("ok") is False and probe.get("error") == "no_model_list",
           f"test without /models: {probe}")

    for method in ("GET", "PUT"):
        status, _, _ = api.call(method, "/llm", {} if method == "PUT" else None)
        expect(status == 404, f"{method} /llm -> {status}")


def load_wisp_scene():
    """tools/wisp_scene.py as a module, for its pack()."""
    spec = importlib.util.spec_from_file_location("wisp_scene", WISP_SCENE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def zip_names(blob):
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        return sorted(z.namelist())


def check_scenes(api, sim):
    """/scenes (scene-runtime-core spec 7.1): uploads the counter example,
    lists, opens, reloads and closes it, and exports it with and without
    its data; a package that does not compile is refused with its file and
    line."""
    zip_header = {"Content-Type": "application/zip"}
    blob, _, _ = load_wisp_scene().pack(COUNTER)
    status, _, text = api.call("POST", "/scenes", headers=zip_header, raw=blob)
    expect(status == 201, f"POST /scenes -> {status} {text}")
    item = json.loads(text)
    expect(item.get("id") == "counter" and item.get("state") == "stopped"
           and item.get("hosts") == [], f"POST /scenes: {item}")
    listed = api.ok_json("GET", "/scenes")["scenes"]
    expect([s["id"] for s in listed] == ["counter"], f"GET /scenes: {listed}")

    for action, state in (("open", "running"), ("reload", "running")):
        got = api.ok_json("POST", f"/scenes/counter/{action}", {})
        expect(got == {"state": state}, f"POST /scenes/counter/{action}: {got}")
    # A click writes the count to the scene's data.
    click(sim, "plus")
    got = api.ok_json("POST", "/scenes/counter/close", {})
    expect(got == {"state": "stopped"}, f"POST /scenes/counter/close: {got}")

    names = {}
    for data in ("0", "1"):
        status, headers, body = api.call("GET", f"/scenes/counter/export?data={data}",
                                         binary=True)
        expect(status == 200 and headers.get("content-type") == "application/zip",
               f"GET /scenes/counter/export?data={data} -> {status} {headers}")
        names[data] = zip_names(body)
    expect(names["0"] == ["main.lua", "scene.json"], f"export without data: {names['0']}")
    expect(any(n.startswith("data/") for n in names["1"])
           and set(names["0"]) < set(names["1"]), f"export with data: {names['1']}")

    bad = io.BytesIO()
    with zipfile.ZipFile(bad, "w") as z:
        z.writestr("scene.json", json.dumps({"id": "bad", "name": "Bad", "version": "1.0.0",
                                             "schema_version": 1}))
        z.writestr("main.lua", "local x = 1\nreturn ui.screen {\n")
    status, _, text = api.call("POST", "/scenes", headers=zip_header, raw=bad.getvalue())
    err = json.loads(text).get("error", {}) if status == 400 else {}
    expect(err.get("code") == "invalid_scene" and err.get("file") == "main.lua"
           and err.get("line") == 3, f"POST /scenes with a syntax error -> {status} {text}")
    listed = api.ok_json("GET", "/scenes")["scenes"]
    expect([s["id"] for s in listed] == ["counter"], f"GET /scenes after the refusal: {listed}")


def check_ws_log(port, cookie):
    """Returns every text message received."""
    ws = WsClient(port, f"wisp_token={cookie}")
    texts = []
    try:
        ws.sock.settimeout(WS_LOG_TIMEOUT_S)
        ws.send_text(json.dumps({"sub": ["log", "events"]}))
        deadline = time.monotonic() + WS_LOG_TIMEOUT_S
        got_log = False
        while not got_log:
            expect(time.monotonic() < deadline, f"no log message within {WS_LOG_TIMEOUT_S}s")
            text = ws.recv_text()
            expect(text is not None, f"the device closed the socket; got {texts}")
            texts.append(text)
            got_log = json.loads(text).get("t") == "log"
        # Collect the rest of the replayed log for the secret scan.
        ws.sock.settimeout(1)
        try:
            while (text := ws.recv_text()) is not None:
                texts.append(text)
        except TimeoutError:
            pass
    finally:
        ws.close()
    return texts


def check_guards(api):
    status, _, text = api.call("POST", "/pair/start", {}, csrf=False)
    expect(status == 403 and '"csrf"' in text, f"POST without X-Wisp -> {status} {text}")
    for path in ("/info", "/device"):
        status, _, text = api.call("GET", path, headers={"Host": "evil.com"})
        expect(status == 403 and '"bad_host"' in text, f"GET {path} as evil.com -> {status} {text}")


def check_pairing_lock(api):
    # The real code was consumed by pairing, so every guess is wrong; the
    # fifth consecutive failure locks (spec 6.1).
    for attempt in range(1, 7):
        status, headers, text = api.call("POST", "/pair/confirm", {"code": "000000"}, auth=False)
        if status == 429:
            break
        expect(status == 403, f"wrong code #{attempt} -> {status} {text}")
    expect(status == 429, f"no 429 after {attempt} wrong codes")
    retry_after = headers.get("retry-after", "")
    expect(retry_after.isdigit() and int(retry_after) >= 1, f"Retry-After: {retry_after!r}")
    body = json.loads(text)
    expect(body["error"].get("retry_after_s") == int(retry_after), f"429 body: {text}")
    status, headers, text = api.call("POST", "/pair/start", {})
    expect(status == 429 and "retry-after" in headers, f"/pair/start while locked -> {status}")
    # Locking code pairing leaves paired browsers alone.
    api.ok_json("GET", "/device")


def check_secrets(api, ws_texts, sim, code, token):
    out, err = sim.output()
    scanned = {"HTTP responses": "\n".join(api.seen), "WebSocket messages": "\n".join(ws_texts),
               "wisp_sim stdout": out, "wisp_sim stderr": err}
    for where, text in scanned.items():
        expect(API_KEY not in text and "wrong-key" not in text, f"an API key is in the {where}")
        # Only the Set-Cookie that delivered it may carry the token.
        allowed = 1 if where == "HTTP responses" else 0
        expect(text.count(token) == allowed,
               f"the pairing token is in the {where} {text.count(token)} time(s)")
    code_lines = [l for l in (out + "\n" + err).splitlines()
                  if re.search(rf"\b{code}\b", l) and l != f"pair-code={code}"]
    expect(not code_lines, f"the pairing code was printed: {code_lines}")
    code_bodies = [s for s in api.seen if re.search(rf"\b{code}\b", s.split(" -> ", 1)[1])]
    code_bodies += [t for t in ws_texts if re.search(rf"\b{code}\b", t)]
    expect(not code_bodies, f"the pairing code was in a response: {code_bodies}")


def stop_sim(sim):
    sim.proc.send_signal(signal.SIGTERM)
    try:
        sim.proc.wait(timeout=STEP_TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"wisp_sim ignored SIGTERM:\n{sim.tail()}") from exc
    sim.join_output()
    expect(sim.proc.returncode == 0,
           f"exit code {sim.proc.returncode} after SIGTERM:\n{sim.output()[1]}")


def run(sim_path, tmp_root):
    fake = sim = None
    try:
        fake = Child([sys.executable, "-I", str(FAKE_OPENAI), "--port", "0", "--exit-on-eof"],
                     stdin=True)
        line = fake.wait_line(lambda l: l.startswith("FAKE_OPENAI "), STEP_TIMEOUT_S)
        expect(line is not None, f"fake_openai.py did not start: {fake.output()[1]}")
        llm_port = json.loads(line[len("FAKE_OPENAI "):])["port"]

        sim, port = start_sim(sim_path, tmp_root / "data")
        api = Api(port)
        code, token = pair(api, sim)
        print("PASS pairing")
        check_routes(api)
        print("PASS routes")
        check_models(api, llm_port)
        print("PASS models")
        check_scenes(api, sim)
        print("PASS scenes")
        ws_texts = check_ws_log(port, token)
        print("PASS websocket log")
        check_guards(api)
        print("PASS csrf and host guards")
        check_pairing_lock(api)
        print("PASS pairing lock")
        stop_sim(sim)
        print("PASS sigterm")
        check_secrets(api, ws_texts, sim, code, token)
        print("PASS no secrets")
    except OSError as exc:
        tail = sim.tail() if sim else ""
        raise SmokeError(f"{type(exc).__name__}: {exc}; last wisp_sim output:\n{tail}") from exc
    finally:
        for child in (sim, fake):
            if child is not None:
                child.kill()


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <path-to-wisp_sim>", file=sys.stderr)
        return 2
    sim_path = pathlib.Path(argv[1]).resolve()
    if not sim_path.is_file():
        print(f"{sim_path}: not found", file=sys.stderr)
        return 2
    tmp_root = pathlib.Path(tempfile.mkdtemp(prefix="wisp_admin_smoke_"))
    try:
        run(sim_path, tmp_root)
    except Exception as exc:  # noqa: BLE001 -- report any failure readably
        print(f"FAIL: {exc}", file=sys.stderr)
        print(traceback.format_exc(), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
