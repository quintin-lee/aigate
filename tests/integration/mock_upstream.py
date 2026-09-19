#!/usr/bin/env python3
"""Mock upstream server mimicking an OpenAI-compatible API for integration testing."""

import http.server
import json
import socketserver
import threading
from typing import List, Dict, Any

import time

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

        if self.path in ("/messages", "/v1/messages"):
            if body_json and body_json.get("stream") is True:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()

                # 1. message_start
                m_start = {
                    "type": "message_start",
                    "message": {
                        "id": "msg_mock_stream_123",
                        "type": "message",
                        "role": "assistant",
                        "model": body_json.get("model", "claude-3-5-sonnet"),
                        "usage": {"input_tokens": 15, "output_tokens": 1}
                    }
                }
                self.wfile.write(f"event: message_start\ndata: {json.dumps(m_start)}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.01)

                # 2. content_block_delta 1
                cbd1 = {
                    "type": "content_block_delta",
                    "index": 0,
                    "delta": {"type": "text_delta", "text": "Hello from "}
                }
                self.wfile.write(f"event: content_block_delta\ndata: {json.dumps(cbd1)}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.01)

                # 3. content_block_delta 2
                cbd2 = {
                    "type": "content_block_delta",
                    "index": 0,
                    "delta": {"type": "text_delta", "text": "Anthropic Claude!"}
                }
                self.wfile.write(f"event: content_block_delta\ndata: {json.dumps(cbd2)}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.01)

                # 4. message_delta
                md = {
                    "type": "message_delta",
                    "delta": {"stop_reason": "end_turn"},
                    "usage": {"output_tokens": 22}
                }
                self.wfile.write(f"event: message_delta\ndata: {json.dumps(md)}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.01)

                # 5. message_stop
                m_stop = {"type": "message_stop"}
                self.wfile.write(f"event: message_stop\ndata: {json.dumps(m_stop)}\n\n".encode("utf-8"))
                self.wfile.flush()
                return
            else:
                # Non-streaming Claude
                response = {
                    "id": "msg_mock_nonstream_123",
                    "type": "message",
                    "role": "assistant",
                    "model": body_json.get("model", "claude-3-5-sonnet") if body_json else "claude-3-5-sonnet",
                    "content": [
                        {
                            "type": "text",
                            "text": "Hello from Anthropic mock!"
                        }
                    ],
                    "stop_reason": "end_turn",
                    "usage": {
                        "input_tokens": 14,
                        "output_tokens": 26
                    }
                }
                resp_bytes = json.dumps(response).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp_bytes)))
                self.end_headers()
                self.wfile.write(resp_bytes)
                return

        if ":streamGenerateContent" in self.path or "alt=sse" in self.path:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()

            c1 = {
                "candidates": [{
                    "content": {"parts": [{"text": "Hello from Gemini "}], "role": "model"},
                    "index": 0
                }]
            }
            self.wfile.write(f"data: {json.dumps(c1)}\n\n".encode("utf-8"))
            self.wfile.flush()
            time.sleep(0.01)

            c2 = {
                "candidates": [{
                    "content": {"parts": [{"text": "stream!"}], "role": "model"},
                    "finishReason": "STOP",
                    "index": 0
                }],
                "usageMetadata": {
                    "promptTokenCount": 11,
                    "candidatesTokenCount": 7,
                    "totalTokenCount": 18
                }
            }
            self.wfile.write(f"data: {json.dumps(c2)}\n\n".encode("utf-8"))
            self.wfile.flush()
            return

        if ":generateContent" in self.path:
            response = {
                "candidates": [{
                    "content": {
                        "parts": [{"text": "Hello from Gemini non-stream!"}],
                        "role": "model"
                    },
                    "finishReason": "STOP",
                    "index": 0
                }],
                "usageMetadata": {
                    "promptTokenCount": 9,
                    "candidatesTokenCount": 5,
                    "totalTokenCount": 14
                }
            }
            resp_bytes = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            return

        if ":embedContent" in self.path:
            response = {
                "embedding": {
                    "values": [0.05, 0.15, 0.25]
                },
                "usageMetadata": {
                    "promptTokenCount": 6
                }
            }
            resp_bytes = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            return

        if ":batchEmbedContents" in self.path:
            response = {
                "embeddings": [
                    {"values": [0.05, 0.15]},
                    {"values": [0.25, 0.35]}
                ],
                "usageMetadata": {
                    "promptTokenCount": 12
                }
            }
            resp_bytes = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            return

        if self.path in ("/embeddings", "/v1/embeddings"):
            response = {
                "object": "list",
                "data": [{
                    "object": "embedding",
                    "index": 0,
                    "embedding": [0.01, 0.02, 0.03]
                }],
                "model": body_json.get("model", "text-embedding-3-small") if body_json else "text-embedding-3-small",
                "usage": {
                    "prompt_tokens": 8,
                    "total_tokens": 8
                }
            }
            resp_bytes = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)
            return

        if self.path in ("/chat/completions", "/v1/chat/completions"):
            req_model = body_json.get("model", "mock-model") if body_json else "mock-model"
            is_deepseek = "deepseek" in req_model.lower()

            if body_json and body_json.get("stream") is True:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()

                if is_deepseek:
                    # DeepSeek streaming chunk with reasoning_content
                    c1 = {
                        "id": "chatcmpl-ds-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [{"index": 0, "delta": {"role": "assistant", "reasoning_content": "DeepSeek reasoning..."}, "finish_reason": None}],
                    }
                    self.wfile.write(f"data: {json.dumps(c1)}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.01)

                    c2 = {
                        "id": "chatcmpl-ds-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [{"index": 0, "delta": {"content": "DeepSeek answer!"}, "finish_reason": "stop"}],
                    }
                    self.wfile.write(f"data: {json.dumps(c2)}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.01)

                    c3 = {
                        "id": "chatcmpl-ds-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [],
                        "usage": {"prompt_tokens": 20, "completion_tokens": 10, "prompt_cache_hit_tokens": 15, "total_tokens": 30},
                    }
                    self.wfile.write(f"data: {json.dumps(c3)}\n\n".encode("utf-8"))
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                    return
                else:
                    # Chunk 1
                    c1 = {
                        "id": "chatcmpl-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [{"index": 0, "delta": {"role": "assistant", "content": "Hello "}, "finish_reason": None}],
                    }
                    self.wfile.write(f"data: {json.dumps(c1)}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.01)

                    # Chunk 2
                    c2 = {
                        "id": "chatcmpl-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [{"index": 0, "delta": {"content": "from stream!"}, "finish_reason": "stop"}],
                    }
                    self.wfile.write(f"data: {json.dumps(c2)}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.01)

                    # Usage chunk
                    c3 = {
                        "id": "chatcmpl-stream",
                        "object": "chat.completion.chunk",
                        "created": 1726700000,
                        "model": req_model,
                        "choices": [],
                        "usage": {"prompt_tokens": 8, "completion_tokens": 12, "total_tokens": 20},
                    }
                    self.wfile.write(f"data: {json.dumps(c3)}\n\n".encode("utf-8"))
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                    return

            msg = {
                "role": "assistant",
                "content": "DeepSeek answer from mock!" if is_deepseek else "Hello from mock upstream!"
            }
            if is_deepseek:
                msg["reasoning_content"] = "Thinking deeply..."

            usage = {
                "prompt_tokens": 20 if is_deepseek else 7,
                "completion_tokens": 10 if is_deepseek else 11,
                "total_tokens": 30 if is_deepseek else 18
            }
            if is_deepseek:
                usage["prompt_cache_hit_tokens"] = 15

            response = {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "created": 1726700000,
                "model": req_model,
                "choices": [{
                    "index": 0,
                    "message": msg,
                    "finish_reason": "stop"
                }],
                "usage": usage
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
