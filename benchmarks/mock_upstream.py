#!/usr/bin/env python3
"""High-performance mock upstream server for aigate benchmarking."""

import argparse
import json
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

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
        # Read request body if present
        clen = int(self.headers.get("Content-Length", 0))
        if clen > 0:
            _ = self.rfile.read(clen)

        if self.path.endswith("/sync"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(SYNC_RESPONSE)))
            self.send_header("X-Upstream-Provider", "mock-primary")
            self.end_headers()
            self.wfile.write(SYNC_RESPONSE)

        elif self.path.endswith("/stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("X-Upstream-Provider", "mock-primary")
            self.end_headers()

            for chunk in STREAM_CHUNKS:
                self.wfile.write(chunk)
                self.wfile.flush()
                time.sleep(0.005)  # 5ms token interval simulation

        elif self.path.endswith("/fail"):
            # Simulate 503 upstream service outage
            err = b'{"error":{"message":"Simulated upstream failure","type":"api_error","code":"service_unavailable"}}'
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.send_header("X-Upstream-Provider", "mock-primary")
            self.end_headers()
            self.wfile.write(err)

        elif self.path.endswith("/backup"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(SYNC_RESPONSE)))
            self.send_header("X-Upstream-Provider", "mock-secondary")
            self.end_headers()
            self.wfile.write(SYNC_RESPONSE)

        else:
            self.send_response(404)
            self.end_headers()


def run(port=19090):
    server = HTTPServer(("127.0.0.1", port), MockHandler)
    print(f"Mock upstream server running on http://127.0.0.1:{port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="aigate Mock Upstream Server")
    parser.add_argument("--port", type=int, default=19090, help="Listen port (default 19090)")
    args = parser.parse_args()
    run(args.port)
