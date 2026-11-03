#!/usr/bin/env python3
"""A stand-in for an OpenAI-compatible endpoint, for the admin smoke test.

Only GET /v1/models is implemented: with "Authorization: Bearer test-key"
it answers {"data":[{"id":"fake-1"},{"id":"fake-2"}]}, otherwise 401.
Every other path is 404. Error bodies never quote the request, so the key
cannot leak back through them. Standard library only.

Usage: python3 tools/fake_openai.py --port <n> [--exit-on-eof]

--port 0 lets the OS pick a free port. Either way the listening port is
printed as one line, FAKE_OPENAI {"port": <n>}, once the socket is bound,
so a caller can start this with --port 0 and read the port back without
racing anyone for it. Runs until killed, or with --exit-on-eof also until
its stdin reaches end of file: a caller that hands it a pipe and then dies
(even by SIGKILL) closes that pipe, so the server never outlives it.
"""

import argparse
import http.server
import json
import os
import sys
import threading

API_KEY = "test-key"
MODELS = {"data": [{"id": "fake-1"}, {"id": "fake-2"}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def _send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 (http.server's naming)
        if self.path != "/v1/models":
            self._send_json(404, {"error": {"message": "not found"}})
        elif self.headers.get("Authorization") != f"Bearer {API_KEY}":
            self._send_json(401, {"error": {"message": "invalid api key"}})
        else:
            self._send_json(200, MODELS)

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
    parser.add_argument("--exit-on-eof", action="store_true",
                        help="exit once stdin reaches end of file")
    args = parser.parse_args(argv[1:])
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
