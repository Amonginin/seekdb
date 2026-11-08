#!/usr/bin/env python3
"""Deterministic AI service mock for the seekdb ai_function mysqltests.

The ai_function tests call the AI functions against an endpoint on
``0.0.0.0:8000``: an OpenAI compatible completion and embedding service, a
SiliconFlow style rerank service and the native Ollama endpoints. This mock
answers all of them with fixed content, so that a test asserts on the shape
of the answer and never on the answer of a real model.

Usage:
    python3 ai_function_mock.py start [port]
    python3 ai_function_mock.py stop  [port]
"""

import json
import os
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

DEFAULT_PORT = 8000
EMBED_DIMENSION = 8


def state_dir(port):
    return "/tmp/seekdb_ai_function_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def deterministic_vector(text, dimension=EMBED_DIMENSION):
    """A vector that depends on the text, so a caller can tell two rows apart."""

    vector = [0.0] * dimension
    for index, word in enumerate(str(text).lower().split()):
        vector[(hash(word) + index) % dimension] += 1.0
    norm = sum(value * value for value in vector) ** 0.5
    if norm == 0.0:
        vector[0] = 1.0
        return vector
    return [value / norm for value in vector]


def build_response(path, body):
    """The answer a request path deserves."""

    if path.endswith("/chat/completions"):
        messages = body.get("messages") or []
        prompt = ""
        for message in messages:
            if message.get("role") == "user":
                prompt = str(message.get("content", ""))
        return {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "model": body.get("model", "mock-model"),
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": "mock completion for %s" % prompt},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": len(prompt.split()), "completion_tokens": 4, "total_tokens": len(prompt.split()) + 4},
        }

    if path.endswith("/embeddings"):
        values = body.get("input", "")
        if not isinstance(values, list):
            values = [values]
        # A caller may ask for a dimension of its own, and the answer has to
        # carry that many numbers.
        dimension = int(body.get("dimensions") or body.get("dimension") or EMBED_DIMENSION)
        return {
            "object": "list",
            "model": body.get("model", "mock-model"),
            "data": [
                {"object": "embedding", "index": index, "embedding": deterministic_vector(value, dimension)}
                for index, value in enumerate(values)
            ],
            "usage": {"prompt_tokens": 1, "total_tokens": 1},
        }

    if path.endswith("/rerank"):
        documents = body.get("documents") or []
        query = str(body.get("query", ""))
        scored = []
        for index, document in enumerate(documents):
            text = document if isinstance(document, str) else str(document)
            # The score only has to order the documents, so a document that
            # shares a word with the query comes first.
            overlap = len(set(query.lower().split()) & set(text.lower().split()))
            scored.append({"index": index, "document": text, "relevance_score": float(overlap)})
        scored.sort(key=lambda item: (-item["relevance_score"], item["index"]))
        return {"id": "rerank-mock", "model": body.get("model", "mock-model"), "results": scored}

    if path.endswith("/api/generate"):
        prompt = str(body.get("prompt", ""))
        return {
            "model": body.get("model", "mock-model"),
            "created_at": "2026-01-01T00:00:00Z",
            "response": "mock ollama generation for %s" % prompt,
            "done": True,
            "done_reason": "stop",
        }

    if path.endswith("/api/embed"):
        # Ollama names the field "embeddings" and it holds a list.
        values = body.get("input", "")
        if not isinstance(values, list):
            values = [values]
        dimension = int(body.get("dimensions") or body.get("dimension") or EMBED_DIMENSION)
        return {
            "model": body.get("model", "mock-model"),
            "embeddings": [deterministic_vector(value, dimension) for value in values],
        }

    return None


def serve(port):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            # Every request is kept, so that a test can tell what the server
            # actually asked for.
            with open(state_path(port, "requests.jsonl"), "a", encoding="utf-8") as stream:
                stream.write(json.dumps({"path": parsed.path, "body": body}) + "\n")
            response = build_response(parsed.path, body)
            if response is None:
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            payload = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.serve_forever()


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    pid = os.fork()
    if pid == 0:
        try:
            serve(port)
        finally:
            os._exit(0)
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(pid))
    for _ in range(100):
        try:
            import socket

            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.05)
    return pid


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    action = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT
    if action == "start":
        pid = start_server(port)
        print("ai_function mock listening on 0.0.0.0:%d (pid %d)" % (port, pid))
    elif action == "stop":
        stop_server(port)
        print("ai_function mock on port %d stopped" % port)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
