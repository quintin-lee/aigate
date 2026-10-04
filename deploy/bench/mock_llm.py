#!/usr/bin/env python3
"""
Lightweight Mock OpenAI Upstream Server for Benchmarking and E2E Testing.
Supports both non-streaming JSON and streaming Server-Sent Events (SSE).
"""
import json
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# Default request_queue_size=5 overflows the SYN backlog under load and
# produces ~1s TCP retransmit outliers; raise it for benchmarking.
ThreadingHTTPServer.request_queue_size = 4096

class MockLLMHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length).decode('utf-8') if content_length > 0 else '{}'
        try:
            req_data = json.loads(body)
        except Exception:
            req_data = {}

        model = req_data.get('model', 'gpt-4o')
        is_stream = req_data.get('stream', False)

        if is_stream:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()

            def send_chunk(text):
                chunk = f"{len(text):X}\r\n{text}\r\n".encode('utf-8')
                self.wfile.write(chunk)
                self.wfile.flush()

            # First chunk (role)
            chunk1 = {
                "id": "chatcmpl-mock-123",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": model,
                "choices": [{"index": 0, "delta": {"role": "assistant", "content": ""}, "finish_reason": None}]
            }
            send_chunk(f"data: {json.dumps(chunk1)}\n\n")

            # Simulate TTFT delay (e.g. 50ms)
            time.sleep(0.05)

            # Content chunks
            words = ["Hello", " from", " local", " aigate", " end-to-end", " streaming", " test!"]
            for w in words:
                chunk = {
                    "id": "chatcmpl-mock-123",
                    "object": "chat.completion.chunk",
                    "created": int(time.time()),
                    "model": model,
                    "choices": [{"index": 0, "delta": {"content": w}, "finish_reason": None}]
                }
                send_chunk(f"data: {json.dumps(chunk)}\n\n")
                time.sleep(0.02)

            # Final stop chunk
            stop_chunk = {
                "id": "chatcmpl-mock-123",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": model,
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 12, "completion_tokens": 18, "total_tokens": 30}
            }
            send_chunk(f"data: {json.dumps(stop_chunk)}\n\n")
            send_chunk("data: [DONE]\n\n")

            # Terminate chunked transfer
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        else:
            resp_body = {
                "id": "chatcmpl-mock-123",
                "object": "chat.completion",
                "created": int(time.time()),
                "model": model,
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": "Hello from local aigate end-to-end non-streaming test!"
                    },
                    "finish_reason": "stop"
                }],
                "usage": {
                    "prompt_tokens": 12,
                    "completion_tokens": 18,
                    "total_tokens": 30
                }
            }
            resp_bytes = json.dumps(resp_body).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            self.wfile.flush()

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        resp = b'{"status":"mock_upstream_ready"}'
        self.send_header('Content-Length', str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, format, *args):
        # Silence per-request access logs during high-volume benchmarks
        pass

if __name__ == '__main__':
    server = ThreadingHTTPServer(('0.0.0.0', 9099), MockLLMHandler)
    server.daemon_threads = True
    print("Mock LLM upstream listening on http://0.0.0.0:9099 ...")
    server.serve_forever()
