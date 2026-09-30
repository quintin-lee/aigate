#!/usr/bin/env python3
"""Low-level raw TCP/HTTP Chaos Upstream Server for aigate."""

import argparse
import signal
import socket
import socketserver
import struct
import sys
import time

HEALTH_RESP = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"status\":\"ok\"}"

SYNC_PAYLOAD = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    b"Content-Length: 138\r\n"
    b"\r\n"
    b'{"id":"chatcmpl-chaos","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]}'
)

SSE_CHUNKS = [
    b'data: {"id":"c-1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"}}]}\n\n',
    b'data: {"id":"c-2","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Chaos"}}]}\n\n',
    b'data: {"id":"c-3","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":" stream"}}]}\n\n',
    b'data: [DONE]\n\n',
]


class ChaosTCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(10.0)
        try:
            req_data = b""
            while b"\r\n\r\n" not in req_data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                req_data += chunk

            headers_part, _, body_part = req_data.partition(b"\r\n\r\n")
            first_line = headers_part.split(b"\r\n")[0].decode("utf-8", errors="ignore")
            path = first_line.split(" ")[1] if len(first_line.split(" ")) > 1 else "/"

            clen = 0
            for line in headers_part.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    try:
                        clen = int(line.split(b":", 1)[1].strip())
                    except Exception:
                        pass

            bytes_left = clen - len(body_part)
            while bytes_left > 0 and "/blackhole" not in path:
                c = sock.recv(min(4096, bytes_left))
                if not c:
                    break
                bytes_left -= len(c)

            action = path

            if "/health" in path:
                sock.sendall(HEALTH_RESP)
                return

            if "/chaos/slow-header" in action:
                # C-01: Trickle header slowly
                sock.sendall(b"HTTP/1.1 200 OK\r\n")
                headers = [b"Content-Type: application/json\r\n", b"X-Chaos: slow\r\n", b"\r\n"]
                for h in headers:
                    for b in h:
                        sock.sendall(bytes([b]))
                        time.sleep(0.04)
                sock.sendall(b'{"status":"slow-ok"}')

            elif "/chaos/slow-stream" in action:
                # C-02: Trickle SSE chunks with 0.1s delay
                headers = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: text/event-stream; charset=utf-8\r\n"
                    b"Cache-Control: no-cache\r\n"
                    b"Connection: close\r\n\r\n"
                )
                sock.sendall(headers)
                for c in SSE_CHUNKS:
                    sock.sendall(c)
                    time.sleep(0.1)

            elif "/chaos/drop-stream" in action:
                # C-03: Sudden TCP drop / RST mid-stream
                headers = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: text/event-stream; charset=utf-8\r\n"
                    b"Cache-Control: no-cache\r\n\r\n"
                )
                sock.sendall(headers)
                # Send first 2 chunks
                sock.sendall(SSE_CHUNKS[0])
                sock.sendall(SSE_CHUNKS[1])
                time.sleep(0.05)
                # Force TCP RST using SO_LINGER
                try:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                except Exception:
                    pass
                sock.close()
                return

            elif "/chaos/bad-chunked" in action:
                # C-05: Corrupted Transfer-Encoding: chunked
                headers = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Transfer-Encoding: chunked\r\n"
                    b"Content-Type: application/json\r\n\r\n"
                )
                sock.sendall(headers)
                # Send valid chunk of 5 bytes
                sock.sendall(b"5\r\nHello\r\n")
                # Send illegal hex chunk length
                sock.sendall(b"ZZZZ\r\nCorruptedPayload\r\n")

            elif "/chaos/truncated-json" in action:
                # C-06: Content-Length mismatch / truncated mid-JSON
                headers = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json\r\n"
                    b"Content-Length: 150\r\n\r\n"
                )
                sock.sendall(headers)
                sock.sendall(b'{"id":"chatcmpl-trunc","choices":[{"delta":{"content":"incomplete')
                sock.close()
                return

            elif "/chaos/blackhole" in action:
                # Hang connection until timeout
                time.sleep(15.0)

            else:
                # Default normal response
                sock.sendall(SYNC_PAYLOAD)

        except (socket.error, ConnectionResetError, BrokenPipeError):
            pass
        finally:
            try:
                sock.close()
            except Exception:
                pass


class ChaosServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def run_server(port=19098):
    server = ChaosServer(("127.0.0.1", port), ChaosTCPHandler)
    print(f"[chaos-server] Listening on http://127.0.0.1:{port}", flush=True)

    def handle_sig(sig, frame):
        try:
            server.server_close()
        except Exception:
            pass
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    try:
        server.serve_forever()
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="aigate Chaos Upstream Server")
    parser.add_argument("--port", type=int, default=19098, help="Listen port (default 19098)")
    args = parser.parse_args()
    run_server(args.port)
