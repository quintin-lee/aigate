#!/usr/bin/env python3
"""High-performance mock upstream server for aigate benchmarking."""

import argparse
import json
import signal
import sys
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

SYNC_RESPONSE = json.dumps({
    "id": "chatcmpl-bench-sync",
    "object": "chat.completion",
    "created": 1700000000,
    "model": "mock-model",
    "choices": [{
        "index": 0,
        "message": {"role": "assistant", "content": "Benchmark sync response payload."},
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 10, "completion_tokens": 6, "total_tokens": 16}
}).encode("utf-8")

STREAM_CHUNKS = [
    b'data: {"id":"chatcmpl-bench-stream","object":"chat.completion.chunk","created":1700000000,"model":"mock-model","choices":[{"index":0,"delta":{"role":"assistant"}}]}\n\n',
    b'data: {"id":"chatcmpl-bench-stream","object":"chat.completion.chunk","created":1700000000,"model":"mock-model","choices":[{"index":0,"delta":{"content":"Hello"}}]}\n\n',
    b'data: {"id":"chatcmpl-bench-stream","object":"chat.completion.chunk","created":1700000000,"model":"mock-model","choices":[{"index":0,"delta":{"content":" benchmark"}}]}\n\n',
    b'data: {"id":"chatcmpl-bench-stream","object":"chat.completion.chunk","created":1700000000,"model":"mock-model","choices":[{"index":0,"delta":{"content":" streaming!"}}]}\n\n',
    b'data: {"id":"chatcmpl-bench-stream","object":"chat.completion.chunk","created":1700000000,"model":"mock-model","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":6,"total_tokens":16}}\n\n',
    b'data: [DONE]\n\n'
]


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # Silence access logging for high-QPS benchmarks
        pass

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "15")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        clen = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(clen) if clen > 0 else b""

        if "/fail" in self.path:
            err = b'{"error":{"message":"Simulated upstream failure","type":"api_error","code":"service_unavailable"}}'
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.send_header("X-Upstream-Provider", "mock-primary")
            self.end_headers()
            self.wfile.write(err)
            return

        is_stream = b'"stream": true' in body_bytes or b'"stream":true' in body_bytes or "/stream" in self.path
        if is_stream:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.send_header("X-Upstream-Provider", "mock-primary")
            self.end_headers()

            for chunk in STREAM_CHUNKS:
                self.wfile.write(chunk)
                self.wfile.flush()
                time.sleep(0.002)
            return

        # Non-streaming sync response
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(SYNC_RESPONSE)))
        provider = "mock-secondary" if "/backup" in self.path else "mock-primary"
        self.send_header("X-Upstream-Provider", provider)
        self.end_headers()
        self.wfile.write(SYNC_RESPONSE)


def run(port=19090):
    server = ThreadingHTTPServer(("127.0.0.1", port), MockHandler)
    print(f"Mock upstream server running on http://127.0.0.1:{port}", flush=True)

    def handle_sig(sig, frame):
        try:
            server.server_close()
        except Exception:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, handle_sig)
    signal.signal(signal.SIGINT, handle_sig)

    try:
        server.serve_forever()
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="aigate Mock Upstream Server")
    parser.add_argument("--port", type=int, default=19090, help="Listen port (default 19090)")
    args = parser.parse_args()
    run(args.port)
