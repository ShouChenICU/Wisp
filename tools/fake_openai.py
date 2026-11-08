#!/usr/bin/env python3
"""A stand-in for an OpenAI-compatible endpoint, for the smoke tests.

Every route checks "Authorization: Bearer test-key" and answers 401
without it. Error bodies never quote the request, so the key cannot leak
back through them, and nothing a request carries is ever printed.
Standard library only.

  GET  /v1/models                {"data":[{"id":"fake-1"},{"id":"fake-2"}]}.
  POST /v1/chat/completions      replays --script as a server-sent event
                                 stream (below); 404 without --script.
  POST /v1/audio/transcriptions  {"text": <--asr-text>} for any upload.
  POST /v1/audio/speech          0.5 s of a 440 Hz tone as raw 16-bit
                                 little-endian mono PCM at the request's
                                 "sample_rate" (24000 when it has none).

Every other path is 404.

The script (--script <json>) is {"steps": [step, ...]}, each step one
model reply: {"text": "..."} and/or {"tool_calls": [{"name": <wire
name>, "arguments": {...}}, ...]}. Which step answers a request depends
only on the request, never on timing: the tool results it carries since
its last user message, counted against the steps' calls in order (a
reply with two calls takes two results to move past). A request past the
last step gets the last one again. Strings in the steps may hold
placeholders, filled in for each request in device local time
(--utc-offset-min, the device's default UTC+8):

  {{tomorrow}}  tomorrow's date, "YYYY-MM-DD";
  {{soon}}      "YYYY-MM-DDTHH:MM", the first whole minute at least 5 s
                from now (so a reminder set for it is due within 65 s).

A reply's text streams a few characters per event and each call's
arguments in pieces, a few milliseconds apart, so a client sees text
split across events and tool calls while they are still being written.
The stream ends with a usage event and "data: [DONE]".

Usage: python3 tools/fake_openai.py --port <n> [--script <json>]
           [--asr-text <text>] [--utc-offset-min <m>] [--exit-on-eof]

--port 0 lets the OS pick a free port. Either way the listening port is
printed as one line, FAKE_OPENAI {"port": <n>}, once the socket is bound,
so a caller can start this with --port 0 and read the port back without
racing anyone for it. Runs until killed, or with --exit-on-eof also until
its stdin reaches end of file: a caller that hands it a pipe and then dies
(even by SIGKILL) closes that pipe, so the server never outlives it.
"""

import argparse
import datetime
import http.server
import json
import math
import os
import struct
import sys
import threading
import time

API_KEY = "test-key"
MODELS = {"data": [{"id": "fake-1"}, {"id": "fake-2"}]}
TTS_DEFAULT_RATE = 24000
TTS_SECONDS = 0.5
TTS_HZ = 440
# A reply's text goes out this many characters per event, and a call's
# arguments in this many pieces, PIECE_GAP_S apart.
TEXT_CHARS_PER_EVENT = 4
ARG_PIECES = 3
PIECE_GAP_S = 0.02
# {{soon}} is at least this far ahead.
SOON_LEAD_S = 5


def fill(value, now, offset_min):
    """`value` with every placeholder in its strings filled in."""
    if isinstance(value, str):
        local = datetime.datetime.fromtimestamp(now, datetime.timezone.utc) + \
            datetime.timedelta(minutes=offset_min)
        tomorrow = (local + datetime.timedelta(days=1)).strftime("%Y-%m-%d")
        soon_s = math.ceil((now + SOON_LEAD_S) / 60) * 60
        soon = (datetime.datetime.fromtimestamp(soon_s, datetime.timezone.utc) +
                datetime.timedelta(minutes=offset_min)).strftime("%Y-%m-%dT%H:%M")
        return value.replace("{{tomorrow}}", tomorrow).replace("{{soon}}", soon)
    if isinstance(value, list):
        return [fill(v, now, offset_min) for v in value]
    if isinstance(value, dict):
        return {k: fill(v, now, offset_min) for k, v in value.items()}
    return value


def pick_step(steps, messages):
    """The step that answers a request with `messages` (see the header)."""
    results = 0
    for m in messages:
        if not isinstance(m, dict):
            continue
        if m.get("role") == "user":
            results = 0
        elif m.get("role") == "tool":
            results += 1
    used = 0
    for step in steps:
        if used >= results:
            return step
        used += len(step.get("tool_calls", []))
    return steps[-1]


def sse_events(step, now, offset_min):
    """The data payloads that stream `step`, [DONE] last."""
    step = fill(step, now, offset_min)
    base = {"id": "chatcmpl-fake", "object": "chat.completion.chunk", "model": "fake-1"}

    def chunk(delta=None, finish=None):
        choice = {"index": 0, "delta": delta or {}, "finish_reason": finish}
        return json.dumps({**base, "choices": [choice]}, ensure_ascii=False)

    yield chunk({"role": "assistant", "content": ""})
    text = step.get("text", "")
    for i in range(0, len(text), TEXT_CHARS_PER_EVENT):
        yield chunk({"content": text[i:i + TEXT_CHARS_PER_EVENT]})
    calls = step.get("tool_calls", [])
    for index, call in enumerate(calls):
        args = json.dumps(call.get("arguments", {}), ensure_ascii=False)
        yield chunk({"tool_calls": [{"index": index, "id": f"call_{index}", "type": "function",
                                     "function": {"name": call["name"], "arguments": ""}}]})
        size = max(1, math.ceil(len(args) / ARG_PIECES))
        for i in range(0, len(args), size):
            yield chunk({"tool_calls": [{"index": index,
                                         "function": {"arguments": args[i:i + size]}}]})
    yield chunk(finish="tool_calls" if calls else "stop")
    yield json.dumps({**base, "choices": [],
                      "usage": {"prompt_tokens": 100, "completion_tokens": 20,
                                "total_tokens": 120}})
    yield "[DONE]"


def tone(rate):
    n = int(rate * TTS_SECONDS)
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * TTS_HZ * i / rate)))
                    for i in range(n))


class Handler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0: every response ends by closing the connection, so a stream
    # needs no length and no chunked framing.
    script = None
    asr_text = ""
    offset_min = 480

    def _send_json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        if self.headers.get("Authorization") == f"Bearer {API_KEY}":
            return True
        self._send_json(401, {"error": {"message": "invalid api key"}})
        return False

    def _body(self):
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def do_GET(self):  # noqa: N802 (http.server's naming)
        if self.path != "/v1/models":
            self._send_json(404, {"error": {"message": "not found"}})
        elif self._authorized():
            self._send_json(200, MODELS)

    def do_POST(self):  # noqa: N802 (http.server's naming)
        routes = {"/v1/chat/completions": self._chat, "/v1/audio/transcriptions": self._asr,
                  "/v1/audio/speech": self._tts}
        route = routes.get(self.path)
        body = self._body()
        if route is None:
            self._send_json(404, {"error": {"message": "not found"}})
        elif self._authorized():
            route(body)

    def _chat(self, body):
        if self.script is None:
            self._send_json(404, {"error": {"message": "no script"}})
            return
        try:
            messages = json.loads(body).get("messages", [])
        except (ValueError, AttributeError):
            self._send_json(400, {"error": {"message": "the body is not a JSON object"}})
            return
        step = pick_step(self.script["steps"], messages)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for data in sse_events(step, time.time(), self.offset_min):
            self.wfile.write(f"data: {data}\n\n".encode())
            self.wfile.flush()
            time.sleep(PIECE_GAP_S)

    def _asr(self, _body):
        self._send_json(200, {"text": self.asr_text})

    def _tts(self, body):
        try:
            rate = int(json.loads(body).get("sample_rate") or TTS_DEFAULT_RATE)
        except (ValueError, AttributeError, TypeError):
            self._send_json(400, {"error": {"message": "the body is not a JSON object"}})
            return
        pcm = tone(rate)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(pcm)))
        self.end_headers()
        self.wfile.write(pcm)

    def log_message(self, format, *args):  # noqa: A002 (overrides the base signature)
        # The default writes every request line to stderr; stay quiet.
        pass


def exit_on_eof():
    while sys.stdin.buffer.read(4096):
        pass
    # serve_forever() is blocking the main thread; end the whole process.
    os._exit(0)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--script", help="the chat replies to play back (JSON)")
    parser.add_argument("--asr-text", default="", help="what every transcription returns")
    parser.add_argument("--utc-offset-min", type=int, default=480,
                        help="the device's UTC offset, for the script's placeholders")
    parser.add_argument("--exit-on-eof", action="store_true",
                        help="exit once stdin reaches end of file")
    args = parser.parse_args(argv[1:])
    if args.script:
        with open(args.script, encoding="utf-8") as f:
            Handler.script = json.load(f)
        if not Handler.script.get("steps"):
            parser.error(f"{args.script}: no steps")
    Handler.asr_text = args.asr_text
    Handler.offset_min = args.utc_offset_min
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("FAKE_OPENAI " + json.dumps({"port": server.server_address[1]}), flush=True)
    if args.exit_on_eof:
        threading.Thread(target=exit_on_eof, daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
