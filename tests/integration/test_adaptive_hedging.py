#!/usr/bin/env python3
"""End-to-end integration tests for adaptive routing and hedged speculative requests."""

import http.server
import json
import threading
import time
import pytest
import requests


class MockLatencyHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress default HTTP request logs in pytest output

    def do_POST(self):
        server: "MockLatencyServer" = self.server
        server.request_count += 1

        content_length = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(content_length)
        body = json.loads(body_bytes.decode("utf-8")) if body_bytes else {}

        # Artificial latency delay
        if server.delay_s > 0:
            time.sleep(server.delay_s)

        model = body.get("model", "test-model")
        is_stream = body.get("stream", False)

        if is_stream:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()

            chunk1 = {
                "id": f"chunk-{server.server_id}",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": model,
                "choices": [{
                    "index": 0,
                    "delta": {"role": "assistant", "content": f"Streaming from {server.server_id}"}
                }]
            }
            self.wfile.write(f"data: {json.dumps(chunk1)}\n\n".encode("utf-8"))
            self.wfile.flush()

            chunk2 = {
                "id": f"chunk-{server.server_id}",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": model,
                "choices": [{
                    "index": 0,
                    "delta": {},
                    "finish_reason": "stop"
                }],
                "usage": {"prompt_tokens": 5, "completion_tokens": 5, "total_tokens": 10}
            }
            self.wfile.write(f"data: {json.dumps(chunk2)}\n\n".encode("utf-8"))
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        else:
            resp_obj = {
                "id": f"chatcmpl-{server.server_id}",
                "object": "chat.completion",
                "created": int(time.time()),
                "model": model,
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": f"Response from {server.server_id}"
                    },
                    "finish_reason": "stop"
                }],
                "usage": {
                    "prompt_tokens": 10,
                    "completion_tokens": 10,
                    "total_tokens": 20
                }
            }
            resp_bytes = json.dumps(resp_obj).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp_bytes)))
            self.end_headers()
            self.wfile.write(resp_bytes)


class MockLatencyServer(http.server.ThreadingHTTPServer):
    def __init__(self, server_id: str, delay_s: float = 0.0):
        super().__init__(("127.0.0.1", 0), MockLatencyHandler)
        self.server_id = server_id
        self.delay_s = delay_s
        self.request_count = 0
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}"

    def close(self):
        self.shutdown()
        self.server_close()


@pytest.fixture
def mock_servers():
    server_a = MockLatencyServer("primary_server", delay_s=0.0)
    server_b = MockLatencyServer("secondary_server", delay_s=0.0)
    yield server_a, server_b
    server_a.close()
    server_b.close()


def test_hedged_tail_latency_reduction_and_metrics(gateway, mock_servers):
    server_a, server_b = mock_servers
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # Primary is slow (1.2s), Secondary is fast (0.02s)
    server_a.delay_s = 1.2
    server_b.delay_s = 0.02
    server_a.request_count = 0
    server_b.request_count = 0

    model_name = "test-hedged-tail-opt"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # Register model with Hedged Requests enabled (delay = 80ms)
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": server_a.url,
            "hedged_enabled": True,
            "hedged_delay_ms": 80,
            "hedge_budget_pct": 100,
            "targets": [
                {"provider": "openai", "endpoint": server_a.url, "priority": 0, "weight": 1},
                {"provider": "openai", "endpoint": server_b.url, "priority": 1, "weight": 1}
            ]
        },
    )
    assert resp.status_code == 201, resp.text
    mdata = resp.json()
    assert mdata["hedged_enabled"] is True
    assert mdata["hedged_delay_ms"] == 80

    # Register API key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "client-hedged-test",
            "allowed_models": [model_name],
            "rate_qps": 50,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Issue request: Primary will take 1.2s, so after 80ms Secondary fires and wins in ~20ms
    t0 = time.time()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "ping"}],
        },
        timeout=5,
    )
    elapsed = time.time() - t0

    assert resp.status_code == 200, resp.text
    data = resp.json()
    # Secondary won the race!
    assert "secondary_server" in data["choices"][0]["message"]["content"]
    # Total latency should be significantly less than 1.2s (typically around 100~300ms)
    assert elapsed < 0.7, f"Elapsed {elapsed}s was not accelerated by hedging"
    assert server_a.request_count == 1
    assert server_b.request_count == 1

    # Verify Prometheus metrics
    m_resp = requests.get(f"{base_url}/metrics")
    assert m_resp.status_code == 200
    metrics_text = m_resp.text
    assert "aigate_hedged_requests_total" in metrics_text
    assert "aigate_hedged_won_total" in metrics_text


def test_hedged_fast_path_zero_redundancy(gateway, mock_servers):
    server_a, server_b = mock_servers
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # Primary is fast (0s delay), Secondary is also fast
    server_a.delay_s = 0.0
    server_b.delay_s = 0.0
    server_a.request_count = 0
    server_b.request_count = 0

    model_name = "test-hedged-zero-redundancy"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # Register model with Hedged Requests enabled (delay = 150ms)
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": server_a.url,
            "hedged_enabled": True,
            "hedged_delay_ms": 150,
            "hedge_budget_pct": 100,
            "targets": [
                {"provider": "openai", "endpoint": server_a.url, "priority": 0, "weight": 1},
                {"provider": "openai", "endpoint": server_b.url, "priority": 1, "weight": 1}
            ]
        },
    )
    assert resp.status_code == 201, resp.text

    # Register API key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "client-zero-red",
            "allowed_models": [model_name],
            "rate_qps": 50,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Send request: Primary finishes in ~5ms (< 150ms delay window)
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "fast"}],
        },
        timeout=3,
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert "primary_server" in data["choices"][0]["message"]["content"]
    assert server_a.request_count == 1
    # Secondary received ZERO requests! Fast-path zero redundancy guaranteed!
    assert server_b.request_count == 0


def test_hedged_streaming_ttft_race(gateway, mock_servers):
    server_a, server_b = mock_servers
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # Server A has delay, Server B is fast
    server_a.delay_s = 0.05
    server_b.delay_s = 0.01

    model_name = "test-hedged-stream-race"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": server_a.url,
            "targets": [
                {"provider": "openai", "endpoint": server_a.url, "priority": 0, "weight": 1},
                {"provider": "openai", "endpoint": server_b.url, "priority": 1, "weight": 1}
            ]
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "client-stream-key",
            "allowed_models": [model_name],
            "rate_qps": 50,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Stream request
    t0 = time.time()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "stream"}],
            "stream": True,
        },
        stream=True,
        timeout=5,
    )
    assert resp.status_code == 200

    chunks = []
    first_chunk_time = None
    for line in resp.iter_lines():
        if line:
            decoded = line.decode("utf-8")
            if decoded.startswith("data: ") and decoded != "data: [DONE]":
                if first_chunk_time is None:
                    first_chunk_time = time.time() - t0
                chunks.append(decoded)

    assert len(chunks) > 0
    assert first_chunk_time is not None
    assert first_chunk_time < 1.0


def test_adaptive_routing_policy_admin_crud(gateway, mock_servers):
    server_a, server_b = mock_servers
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-adaptive-policy-crud"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # 1. Create model with lb_policy = "latency_p95"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": server_a.url,
            "lb_policy": "latency_p95",
            "hedged_enabled": True,
            "hedged_delay_ms": 120,
            "hedge_budget_pct": 25,
            "targets": [
                {"provider": "openai", "endpoint": server_a.url, "priority": 0, "weight": 1},
                {"provider": "openai", "endpoint": server_b.url, "priority": 0, "weight": 1}
            ]
        },
    )
    assert resp.status_code == 201, resp.text
    data = resp.json()
    assert data["hedged_enabled"] is True
    assert data["hedged_delay_ms"] == 120
    assert data["hedge_budget_pct"] == 25

    # 2. List models and check fields
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200
    models = resp.json()["models"]
    m = next((x for x in models if x["name"] == model_name), None)
    assert m is not None
    assert m["lb_policy"] == "latency_p95"
    assert m["hedged_enabled"] is True
    assert m["hedged_delay_ms"] == 120
    assert m["hedge_budget_pct"] == 25

    # 3. Patch model to dynamic_weighted and change hedged parameters
    resp = requests.patch(
        f"{base_url}/admin/v1/models/{model_name}",
        headers=admin_headers,
        json={
            "lb_policy": "dynamic_weighted",
            "hedged_delay_ms": 200,
            "hedge_budget_pct": 10,
        },
    )
    assert resp.status_code == 200, resp.text

    # 4. Verify patch updated correctly
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200
    models = resp.json()["models"]
    m = next((x for x in models if x["name"] == model_name), None)
    assert m is not None
    assert m["lb_policy"] == "dynamic_weighted"
    assert m["hedged_delay_ms"] == 200
    assert m["hedge_budget_pct"] == 10


def test_hedged_budget_limit(gateway, mock_servers):
    server_a, server_b = mock_servers
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    server_a.delay_s = 0.3
    server_b.delay_s = 0.02
    server_a.request_count = 0
    server_b.request_count = 0

    model_name = "test-hedge-budget-limit"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # Register model with hedge_budget_pct = 5 (very strict quota)
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": server_a.url,
            "hedged_enabled": True,
            "hedged_delay_ms": 50,
            "hedge_budget_pct": 5,
            "targets": [
                {"provider": "openai", "endpoint": server_a.url, "priority": 0, "weight": 1},
                {"provider": "openai", "endpoint": server_b.url, "priority": 1, "weight": 1}
            ]
        },
    )
    assert resp.status_code == 201

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "client-budget-key",
            "allowed_models": [model_name],
            "rate_qps": 50,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Request 1: first request hedge is admitted
    resp1 = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "req1"}]},
        timeout=3,
    )
    assert resp1.status_code == 200
    assert server_b.request_count == 1

    # Request 2 immediately: budget is exhausted (1 hedge / 2 total = 50% > 5% budget)
    # Secondary must NOT be spawned!
    resp2 = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "req2"}]},
        timeout=3,
    )
    assert resp2.status_code == 200
    # server_b still has request_count == 1 (not incremented!)
    assert server_b.request_count == 1
    assert "primary_server" in resp2.json()["choices"][0]["message"]["content"]

