#!/usr/bin/env python3
"""Mock upstream server mimicking an OpenAI-compatible API for integration testing."""

import http.server
import json
import socketserver
import threading
from typing import List, Dict, Any

class MockUpstreamHandler(http.server.BaseHTTPRequestHandler):
    recorded_requests: List[Dict[str, Any]] = []

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(content_length)
        body_json = None
        try:
            body_json = json.loads(body_bytes.decode("utf-8")) if body_bytes else None
        except Exception:
            pass

        self.__class__.recorded_requests.append({
            "path": self.path,
            "headers": dict(self.headers),
            "body": body_json,
        })

        if self.path == "/fail":
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error":"simulated internal failure"}')
            return

        if self.path in ("/chat/completions", "/v1/chat/completions"):
            response = {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "created": 1726700000,
                "model": body_json.get("model", "mock-model") if body_json else "mock-model",
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": "Hello from mock upstream!"
                    },
                    "finish_reason": "stop"
                }],
                "usage": {
                    "prompt_tokens": 7,
                    "completion_tokens": 11,
                    "total_tokens": 18
                }
            }
            resp_bytes = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, format, *args):
        pass  # quiet test logs

class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

def start_mock_upstream(port: int = 0) -> tuple[ThreadedHTTPServer, int]:
    server = ThreadedHTTPServer(("127.0.0.1", port), MockUpstreamHandler)
    actual_port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, actual_port

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19999
    server, p = start_mock_upstream(port)
    print(f"Mock upstream listening on port {p}")
    try:
        while True:
            import time
            time.sleep(1)
    except KeyboardInterrupt:
        server.shutdown()
