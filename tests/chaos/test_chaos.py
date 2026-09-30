#!/usr/bin/env python3
"""Automated Chaos and Robustness Test Suite for aigate."""

import argparse
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
BUILD_DIR = os.path.join(ROOT_DIR, ".build")
AIGATE_BIN = os.path.join(BUILD_DIR, "aigate")
CHAOS_SERVER_SCRIPT = os.path.join(ROOT_DIR, "tests", "chaos", "chaos_server.py")

DEFAULT_GATEWAY_PORT = 18088
DEFAULT_CHAOS_PORT = 19098
ADMIN_TOKEN = "aig_admin_chaos_secret"

DEFAULT_PG_CANDIDATES = [
    os.environ.get("AIGATE_PG_DSN", ""),
    "postgresql://aigate:changeme@172.39.4.3:5432/aigate",
    "postgresql://aigate:changeme@172.39.4.2:5432/aigate",
    "postgresql://postgres:postgres@127.0.0.1:5432/aigate_test",
    "postgresql://postgres:postgres@127.0.0.1:5432/aigate",
]


def resolve_pg_dsn():
    for dsn in DEFAULT_PG_CANDIDATES:
        if not dsn:
            continue
        try:
            res = subprocess.run(["pg_isready", "-d", dsn, "-t", "1"], capture_output=True)
            if res.returncode == 0:
                return dsn
        except Exception:
            pass
    return "postgresql://aigate:changeme@172.39.4.3:5432/aigate"


class ChaosTester:
    def __init__(self, gateway_port=DEFAULT_GATEWAY_PORT, chaos_port=DEFAULT_CHAOS_PORT, auto_boot=True, bin_path=None):
        self.gateway_port = gateway_port
        self.chaos_port = chaos_port
        self.auto_boot = auto_boot
        self.bin_path = bin_path or os.environ.get("AIGATE_BIN", AIGATE_BIN)
        self.pg_dsn = resolve_pg_dsn()
        self.gateway_proc = None
        self.chaos_proc = None
        self.api_key = ""

    def setup(self):
        # 1. Clean proxy environment to prevent 127.0.0.1 proxy hijacking
        for k in ["http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"]:
            os.environ.pop(k, None)
        os.environ["no_proxy"] = "127.0.0.1,localhost"
        os.environ["NO_PROXY"] = "127.0.0.1,localhost"

        if self.auto_boot:
            # Start chaos server
            cmd_chaos = [sys.executable, CHAOS_SERVER_SCRIPT, "--port", str(self.chaos_port)]
            self.chaos_proc = subprocess.Popen(cmd_chaos, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self._wait_url(f"http://127.0.0.1:{self.chaos_port}/health", timeout=5, name="Chaos server")

            # Start gateway
            env = os.environ.copy()
            for k in ["http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"]:
                env.pop(k, None)
            env["AIGATE_LISTEN"] = f":{self.gateway_port}"
            env["AIGATE_PG_DSN"] = self.pg_dsn
            env["AIGATE_ADMIN_TOKEN"] = ADMIN_TOKEN
            env["AIGATE_MASTER_KEY"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            env["AIGATE_METRICS_ACL"] = "127.0.0.1"
            env["AIGATE_USAGE_FLUSH_S"] = "1"

            self.gateway_proc = subprocess.Popen(
                [self.bin_path],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL
            )
            self._wait_url(f"http://127.0.0.1:{self.gateway_port}/metrics", timeout=6, name="aigate gateway")

        # 2. Register dynamic models pointing to chaos endpoints
        self.register_test_resources()

    def _wait_url(self, url, timeout=6, name="Service"):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(url, timeout=0.5) as r:
                    if r.status == 200:
                        return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError(f"{name} at {url} not ready within {timeout}s")

    def register_test_resources(self):
        admin_url = f"http://127.0.0.1:{self.gateway_port}/admin/v1"
        admin_headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {ADMIN_TOKEN}"
        }

        models = [
            ("chaos-slow-header", f"http://127.0.0.1:{self.chaos_port}/chaos/slow-header"),
            ("chaos-slow-stream", f"http://127.0.0.1:{self.chaos_port}/chaos/slow-stream"),
            ("chaos-drop-stream", f"http://127.0.0.1:{self.chaos_port}/chaos/drop-stream"),
            ("chaos-bad-chunked", f"http://127.0.0.1:{self.chaos_port}/chaos/bad-chunked"),
            ("chaos-truncated-json", f"http://127.0.0.1:{self.chaos_port}/chaos/truncated-json"),
            ("chaos-blackhole", f"http://127.0.0.1:{self.chaos_port}/chaos/blackhole"),
        ]
        for name, ep in models:
            del_req = urllib.request.Request(
                f"{admin_url}/models/{name}",
                headers=admin_headers,
                method="DELETE"
            )
            try:
                with urllib.request.urlopen(del_req, timeout=2) as resp:
                    _ = resp.read()
            except Exception:
                pass

            payload = json.dumps({"name": name, "provider": "openai", "endpoint": ep}).encode("utf-8")
            req = urllib.request.Request(
                f"{admin_url}/models",
                data=payload,
                headers=admin_headers,
                method="POST"
            )
            try:
                with urllib.request.urlopen(req, timeout=2) as resp:
                    _ = resp.read()
            except Exception:
                pass

        # Register key
        key_payload = json.dumps({
            "name": f"chaos-key-{int(time.time())}",
            "allowed_models": [],
            "rate_qps": 100000,
            "daily_token_quota": 0,
        }).encode("utf-8")
        req = urllib.request.Request(
            f"{admin_url}/keys",
            data=key_payload,
            headers=admin_headers,
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=2) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            self.api_key = data.get("plaintext", "")

    def cleanup(self):
        for proc in [self.gateway_proc, self.chaos_proc]:
            if proc and proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=2)
                except Exception:
                    proc.kill()

    # --- Test Scenarios ---

    def test_c01_slow_header_timeout(self):
        """C-01: Upstream trickles header; gateway should timeout or safely handle without crash."""
        print("[chaos] Testing C-01 (Slow Header)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        payload = json.dumps({"model": "chaos-slow-header", "messages": [{"role": "user", "content": "hi"}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                _ = resp.read()
            print("PASS (responded)")
        except urllib.error.HTTPError as e:
            assert e.code in (500, 502, 504), f"Unexpected status code: {e.code}"
            print(f"PASS (HTTP {e.code})")
        except socket.timeout:
            print("PASS (client timeout safely bounded)")

    def test_c02_slow_stream(self):
        """C-02: Upstream SSE with delayed chunks; verify gateway streams progressively."""
        print("[chaos] Testing C-02 (Slow Stream SSE)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        payload = json.dumps({"model": "chaos-slow-stream", "stream": True, "messages": [{"role": "user", "content": "hi"}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json", "Accept": "text/event-stream"})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=8) as resp:
                data = resp.read().decode("utf-8")
                elapsed = time.time() - t0
                assert "Chaos" in data or "chatcmpl" in data or "data:" in data
                assert elapsed >= 0.1, "Stream should reflect chunk pacing"
            print(f"PASS (elapsed {elapsed:.2f}s)")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="ignore")
            print(f"FAIL (HTTP {e.code}: {body})")
            raise

    def test_c03_upstream_drop(self):
        """C-03: Upstream drops connection abruptly mid-stream."""
        print("[chaos] Testing C-03 (Upstream Sudden TCP Drop)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        payload = json.dumps({"model": "chaos-drop-stream", "stream": True, "messages": [{"role": "user", "content": "hi"}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3) as resp:
                _ = resp.read()
            print("PASS (graceful close)")
        except (urllib.error.HTTPError, urllib.error.URLError, ConnectionResetError, http.client.RemoteDisconnected):
            print("PASS (drop detected safely)")

    def test_c04_client_abort(self):
        """C-04: Client connects, receives first chunk, then closes raw TCP socket immediately."""
        print("[chaos] Testing C-04 (Client Abort / Early Close)... ", end="", flush=True)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", self.gateway_port))
        req_body = json.dumps({"model": "chaos-slow-stream", "stream": True, "messages": [{"role": "user", "content": "hi"}]})
        req = (
            f"POST /v1/chat/completions HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.gateway_port}\r\n"
            f"Authorization: Bearer {self.api_key}\r\n"
            f"Content-Type: application/json\r\n"
            f"Accept: text/event-stream\r\n"
            f"Content-Length: {len(req_body)}\r\n\r\n"
            f"{req_body}"
        )
        s.sendall(req.encode("utf-8"))
        # Read until first chunk arrives
        buf = b""
        s.settimeout(3.0)
        while b"data:" not in buf:
            try:
                chunk = s.recv(1024)
                if not chunk:
                    break
                buf += chunk
            except Exception:
                break
        # Client abruptly disconnects
        s.close()
        time.sleep(0.3)
        # Verify gateway is completely healthy
        with urllib.request.urlopen(f"http://127.0.0.1:{self.gateway_port}/metrics", timeout=2) as resp:
            assert resp.status == 200
        print("PASS (gateway remained stable and responsive)")

    def test_c05_bad_chunked(self):
        """C-05: Upstream sends invalid Transfer-Encoding: chunked frames."""
        print("[chaos] Testing C-05 (Bad Chunked Encoding)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        payload = json.dumps({"model": "chaos-bad-chunked", "messages": [{"role": "user", "content": "hi"}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3) as resp:
                _ = resp.read()
            print("PASS (handled)")
        except urllib.error.HTTPError as e:
            assert e.code in (500, 502), f"Unexpected status code: {e.code}"
            print(f"PASS (HTTP {e.code})")
        except Exception:
            print("PASS (connection handled)")

    def test_c06_truncated_json(self):
        """C-06: Upstream cuts off mid-JSON payload."""
        print("[chaos] Testing C-06 (Truncated JSON)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        payload = json.dumps({"model": "chaos-truncated-json", "messages": [{"role": "user", "content": "hi"}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3) as resp:
                _ = resp.read()
            print("PASS (handled)")
        except urllib.error.HTTPError as e:
            assert e.code in (500, 502), f"Unexpected status code: {e.code}"
            print(f"PASS (HTTP {e.code})")
        except Exception:
            print("PASS (handled safely)")

    def test_c07_oversized_payload(self):
        """C-07: Client sends oversized payload to gateway."""
        print("[chaos] Testing C-07 (Oversized Payload Shield)... ", end="", flush=True)
        url = f"http://127.0.0.1:{self.gateway_port}/v1/chat/completions"
        big_content = "x" * (2 * 1024 * 1024)  # 2MB payload
        payload = json.dumps({"model": "chaos-slow-header", "messages": [{"role": "user", "content": big_content}]}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                _ = resp.read()
            print("PASS (processed)")
        except urllib.error.HTTPError as e:
            assert e.code in (400, 413, 500, 502, 504), f"Unexpected status code: {e.code}"
            print(f"PASS (HTTP {e.code})")
        except Exception:
            print("PASS (safe drop)")

    def run_all(self):
        print("=" * 60)
        print("Starting aigate Chaos & Robustness Testing Suite")
        print("=" * 60)
        self.setup()
        try:
            self.test_c01_slow_header_timeout()
            self.test_c02_slow_stream()
            self.test_c03_upstream_drop()
            self.test_c04_client_abort()
            self.test_c05_bad_chunked()
            self.test_c06_truncated_json()
            self.test_c07_oversized_payload()
            print("=" * 60)
            print("ALL CHAOS TESTS COMPLETED SUCCESSFULLY!")
            print("=" * 60)
            return 0
        finally:
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(description="aigate Chaos Test Runner")
    parser.add_argument("--gateway-port", type=int, default=DEFAULT_GATEWAY_PORT)
    parser.add_argument("--chaos-port", type=int, default=DEFAULT_CHAOS_PORT)
    parser.add_argument("--bin", type=str, default=None, help="Path to aigate binary")
    parser.add_argument("--no-boot", action="store_true", help="Do not boot gateway automatically")
    args = parser.parse_args()

    tester = ChaosTester(
        gateway_port=args.gateway_port,
        chaos_port=args.chaos_port,
        auto_boot=not args.no_boot,
        bin_path=args.bin
    )
    return tester.run_all()


if __name__ == "__main__":
    sys.exit(main())
