#!/usr/bin/env python3

import json
import errno
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class BackendServer(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), BackendHandler)
        self.requests = []
        self.responses = []


class BackendHandler(BaseHTTPRequestHandler):
    server: BackendServer

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > 1024 * 1024:
            self.send_error(400)
            return
        payload = self.rfile.read(length)
        try:
            body = json.loads(payload)
        except json.JSONDecodeError:
            self.send_error(400)
            return
        self.server.requests.append({
            "path": self.path,
            "authorization": self.headers.get("Authorization", ""),
            "content_type": self.headers.get("Content-Type", ""),
            "body": body,
        })
        status, response = self.server.responses.pop(0)
        encoded = response if isinstance(response, bytes) else json.dumps(response).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, _format, *_args):
        return


REQUEST = """protocol\t1
preedit\tnihao
application\tTestEditor
candidates\t你好\t你号
candidate_metadata\t0\tconsumed_prefix\t0\tsource\tlookup\tscore\t100
candidate_metadata\t1\tconsumed_prefix\t0\tsource\tlookup\tscore\t99
state\tpreedit_cursor\t5\tcandidate_cursor\t0\texpanded\t0
runtime_state\tcontinuous\t0
events\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o
correction_events\tletter:i\tletter:h\tletter:a\tletter:o\tbackspace:\tbackspace:\tbackspace:\tbackspace:\tletter:n\tletter:i\tletter:h\tletter:a\tletter:o
"""


def run_adapter(adapter, cache_home, server, *, backend, api_key="", chat_path="/chat/completions"):
    env = dict(os.environ)
    env.update({
        "XDG_CACHE_HOME": str(cache_home),
        "TMPDIR": str(cache_home.parent),
        "TIPE_MODEL_BACKEND": backend,
        "TIPE_MODEL_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
        "TIPE_MODEL_CHAT_PATH": chat_path,
        "TIPE_MODEL_NAME": "tipe-http-test",
        "TIPE_MODEL_API_KEY": api_key,
        "TIPE_MODEL_HTTP_TIMEOUT_SECONDS": "3",
        "TIPE_MODEL_TEMPERATURE": "0.2",
        "TIPE_MODEL_MAX_TOKENS": "96",
    })
    return subprocess.run(
        [str(adapter)],
        input=REQUEST,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )


def main():
    require(len(sys.argv) == 2, "expected model adapter path")
    adapter = Path(sys.argv[1])
    require(adapter.is_file(), f"missing model adapter: {adapter}")

    try:
        server = BackendServer()
    except OSError as error:
        if error.errno in {errno.EACCES, errno.EPERM}:
            print("model backend HTTP integration skipped: local sockets are unavailable", file=sys.stderr)
            return 77
        raise
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="tipe-model-http-") as temporary:
            cache_home = Path(temporary) / "cache"
            server.responses.append((200, {
                "choices": [{
                    "message": {
                        "content": (
                            "candidate\t不存在\n"
                            "candidate\t你号\n"
                            "correction\tihao\tzzzz\n"
                            "correction\tihao\tnihao\n"
                        )
                    }
                }]
            }))
            openai = run_adapter(
                adapter,
                cache_home,
                server,
                backend="openai-compatible",
                api_key="local-test-secret",
                chat_path="/custom/chat",
            )
            require(openai.returncode == 0, f"OpenAI-compatible request failed: {openai.stderr}")
            require(
                openai.stdout == "candidate\t你号\ncorrection\tihao\tnihao\n",
                f"unsafe provider rows were not filtered: {openai.stdout!r}",
            )
            captured = server.requests[-1]
            require(captured["path"] == "/v1/custom/chat", "custom chat path was not used")
            require(
                captured["authorization"] == "Bearer local-test-secret",
                "API key was not sent as a bearer token",
            )
            require(
                not list(cache_home.parent.glob("tipe-model-header.*")),
                "temporary API-key header file was not removed after success",
            )
            require(
                captured["content_type"].startswith("application/json"),
                "request content type is not JSON",
            )
            body = captured["body"]
            require(
                body.get("model") == "tipe-http-test"
                and body.get("temperature") == 0.2
                and body.get("max_tokens") == 96,
                "provider settings were not preserved in the HTTP request",
            )
            prompt = json.loads(body["messages"][1]["content"])
            require(
                prompt.get("protocol") == "tipe.cloud-rerank.v1"
                and prompt.get("invocation") == "explicit-one-shot"
                and prompt.get("preedit") == "nihao"
                and prompt.get("application") == "TestEditor"
                and prompt.get("candidates") == ["你好", "你号"],
                "supervised request context was not serialized correctly",
            )

            server.responses.append((200, {
                "choices": [{"message": {"content": "candidate\t你号\n"}}]
            }))
            ollama = run_adapter(adapter, cache_home, server, backend="ollama")
            require(ollama.returncode == 0 and ollama.stdout == "candidate\t你号\n", ollama.stderr)
            require(
                server.requests[-1]["path"] == "/v1/chat/completions"
                and not server.requests[-1]["authorization"],
                "Ollama-compatible local request used unexpected transport settings",
            )

            server.responses.append((500, {"error": "test failure"}))
            failed = run_adapter(
                adapter, cache_home, server, backend="openai-compatible", api_key="failure-secret"
            )
            require(
                failed.returncode != 0 and not failed.stdout,
                "HTTP failure should return no candidate output and a nonzero status",
            )
            require(
                not list(cache_home.parent.glob("tipe-model-header.*")),
                "temporary API-key header file was not removed after HTTP failure",
            )

            server.responses.append((200, b"not-json"))
            malformed = run_adapter(adapter, cache_home, server, backend="openai-compatible")
            require(
                malformed.returncode == 0 and not malformed.stdout,
                "malformed provider JSON should degrade to an empty safe result",
            )
            request_count = len(server.requests)
            invalid_key = run_adapter(
                adapter,
                cache_home,
                server,
                backend="openai-compatible",
                api_key="bad\nheader",
            )
            require(
                invalid_key.returncode == 2
                and "must not contain newlines" in invalid_key.stderr
                and len(server.requests) == request_count,
                "newline-bearing API keys must be rejected before an HTTP request",
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    require(not thread.is_alive(), "local model test server did not stop")
    print("model backend HTTP integration ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
