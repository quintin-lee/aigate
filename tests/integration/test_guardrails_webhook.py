#!/usr/bin/env python3
"""End-to-end integration tests for External Webhook Content Moderation in AIGate."""

import http.server
import json
import socketserver
import threading
import time
from typing import Generator, Dict, Any, List
import pytest
import requests


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockWebhookHandler(http.server.BaseHTTPRequestHandler):
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

        try:
            if "/slow" in self.path:
                time.sleep(1.0)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"action":"pass"}')
                return

            if "/error" in self.path:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"error":"simulated_webhook_error"}')
                return

            if "/block" in self.path:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"action":"block","reason":"prohibited_by_audit_webhook"}')
                return

            if "/mask" in self.path:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"action":"mask","masked_content":"hello [WEBHOOK_MASKED]"}')
                return

            # default: pass
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"action":"pass"}')
        except BrokenPipeError:
            pass

    def log_message(self, format, *args):
        # Silence HTTP server logs during pytest runs
        pass


@pytest.fixture(scope="module")
def mock_webhook_server() -> Generator[str, None, None]:
    MockWebhookHandler.recorded_requests = []
    server = ThreadedTCPServer(("127.0.0.1", 0), MockWebhookHandler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}"
    server.shutdown()
    server.server_close()


@pytest.fixture(scope="module")
def webhook_client(gateway, mock_webhook_server):
    """Setup a model route and a client key with guardrails enabled."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_upstream = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-webhook-model"
    # 1. Register model pointing to mock upstream
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_upstream,
        },
    )
    assert resp.status_code in (200, 201), resp.text

    # 2. Register API Key with guardrails enabled
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "webhook-test-key",
            "allowed_models": [model_name],
            "guardrails_enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    client_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {client_key}",
        "Content-Type": "application/json",
    }

    return {
        "base_url": base_url,
        "admin_headers": admin_headers,
        "client_headers": client_headers,
        "model_name": model_name,
        "webhook_base": mock_webhook_server,
    }


def test_webhook_admin_crud_and_probe(webhook_client):
    """Test Admin API: webhook rule CRUD and connection probe endpoint."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    webhook_base = webhook_client["webhook_base"]

    # 1. Probe a healthy webhook endpoint
    resp = requests.post(
        f"{base_url}/admin/v1/guardrails/webhook/test",
        headers=admin_headers,
        json={
            "url": f"{webhook_base}/pass",
            "webhook_secret": "whsec-test-token",
            "timeout_ms": 1000,
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["status"] == "ok"
    assert data["reachable"] is True
    assert data["latency_ms"] >= 0

    # 2. Probe an unreachable port
    resp = requests.post(
        f"{base_url}/admin/v1/guardrails/webhook/test",
        headers=admin_headers,
        json={
            "url": "http://127.0.0.1:1/probe",
            "timeout_ms": 100,
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["status"] == "error"
    assert data["reachable"] is False

    # 3. Create a webhook guardrail rule
    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/pass",
            "action": "block",
            "category": "security",
            "webhook_secret": "secret-crud-test",
            "timeout_ms": 350,
            "fail_mode": "closed",
            "phase": "both",
        },
    )
    assert resp.status_code == 201, resp.text
    created = resp.json()
    rule_id = created["id"]
    assert created["rule_type"] == "webhook"
    assert created["webhook_secret"] == "secret-crud-test"
    assert created["timeout_ms"] == 350
    assert created["fail_mode"] == "closed"
    assert created["phase"] == "both"

    # 4. List rules and verify fields
    resp = requests.get(f"{base_url}/admin/v1/guardrails", headers=admin_headers)
    assert resp.status_code == 200
    rules = resp.json()["rules"]
    matching = [r for r in rules if r["id"] == rule_id]
    assert len(matching) == 1
    assert matching[0]["pattern"] == f"{webhook_base}/pass"
    assert matching[0]["timeout_ms"] == 350

    # 5. Update webhook rule
    resp = requests.patch(
        f"{base_url}/admin/v1/guardrails/{rule_id}",
        headers=admin_headers,
        json={
            "timeout_ms": 600,
            "fail_mode": "open",
        },
    )
    assert resp.status_code == 200, resp.text
    updated = resp.json()
    assert updated["timeout_ms"] == 600
    assert updated["fail_mode"] == "open"

    # 6. Cleanup rule
    resp = requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)
    assert resp.status_code == 200


def test_webhook_inbound_pass(webhook_client):
    """Test Inbound Webhook inspection when moderation returns 'pass'."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    # Register inbound webhook pass rule
    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/pass",
            "action": "block",
            "category": "security",
            "webhook_secret": "bearer-token-123",
            "timeout_ms": 500,
            "fail_mode": "closed",
            "phase": "inbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        MockWebhookHandler.recorded_requests.clear()
        # Send chat completion
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Hello, please provide information."}],
            },
        )
        assert resp.status_code == 200, resp.text
        # Verify webhook was invoked with standard payload
        assert len(MockWebhookHandler.recorded_requests) >= 1
        last_req = MockWebhookHandler.recorded_requests[-1]
        assert "/pass" in last_req["path"]
        assert last_req["headers"].get("Authorization") == "Bearer bearer-token-123"
        body = last_req["body"]
        assert body["phase"] == "inbound"
        assert body["model"] == model_name
        assert "Hello, please provide information." in body["content"]
        assert isinstance(body["messages"], list)
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_inbound_block(webhook_client):
    """Test Inbound Webhook inspection when moderation returns 'block'."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/block",
            "action": "block",
            "category": "security",
            "timeout_ms": 500,
            "fail_mode": "closed",
            "phase": "inbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Malicious payload attempt"}],
            },
        )
        assert resp.status_code == 400, resp.text
        err = resp.json()
        assert err["error"]["type"] == "content_policy_violation"
        assert "prohibited_by_audit_webhook" in err["error"]["message"]
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_inbound_mask(webhook_client):
    """Test Inbound Webhook inspection when moderation returns 'mask'."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/mask",
            "action": "mask",
            "category": "privacy",
            "timeout_ms": 500,
            "fail_mode": "open",
            "phase": "inbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        from mock_upstream import MockUpstreamHandler
        MockUpstreamHandler.recorded_requests.clear()

        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Sensitive unmasked content"}],
            },
        )
        assert resp.status_code == 200, resp.text
        # Verify upstream received the sanitized user message
        assert len(MockUpstreamHandler.recorded_requests) >= 1
        upstream_req = MockUpstreamHandler.recorded_requests[-1]
        user_msgs = [m for m in upstream_req["body"].get("messages", []) if m.get("role") == "user"]
        assert len(user_msgs) > 0
        assert "[WEBHOOK_MASKED]" in user_msgs[-1]["content"]
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_timeout_fail_open(webhook_client):
    """Test Webhook timeout with Fail-Open policy: request should succeed."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    # Timeout = 200ms, Webhook takes 1500ms
    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/slow",
            "action": "block",
            "category": "security",
            "timeout_ms": 200,
            "fail_mode": "open",
            "phase": "inbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        t0 = time.time()
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Hello fail-open test"}],
            },
        )
        elapsed = time.time() - t0
        assert resp.status_code == 200, resp.text
        # Should timeout around 200-500ms rather than waiting full 1.5s
        assert elapsed < 1.4
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_timeout_fail_closed(webhook_client):
    """Test Webhook timeout with Fail-Closed policy: request should be blocked."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/slow",
            "action": "block",
            "category": "security",
            "timeout_ms": 200,
            "fail_mode": "closed",
            "phase": "inbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Hello fail-closed test"}],
            },
        )
        assert resp.status_code == 400, resp.text
        err = resp.json()
        assert err["error"]["type"] == "content_policy_violation"
        assert "moderation_webhook_unavailable" in err["error"]["message"]
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_outbound_block(webhook_client):
    """Test Outbound Webhook inspection blocking the LLM model completion."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/block",
            "action": "block",
            "category": "security",
            "timeout_ms": 500,
            "fail_mode": "closed",
            "phase": "outbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Tell me a joke"}],
            },
        )
        assert resp.status_code == 400, resp.text
        err = resp.json()
        assert err["error"]["type"] == "content_policy_violation"
        assert "prohibited_by_audit_webhook" in err["error"]["message"]
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)


def test_webhook_outbound_mask(webhook_client):
    """Test Outbound Webhook inspection masking the LLM model completion."""
    base_url = webhook_client["base_url"]
    admin_headers = webhook_client["admin_headers"]
    client_headers = webhook_client["client_headers"]
    model_name = webhook_client["model_name"]
    webhook_base = webhook_client["webhook_base"]

    resp = requests.post(
        f"{base_url}/admin/v1/guardrails",
        headers=admin_headers,
        json={
            "rule_type": "webhook",
            "pattern": f"{webhook_base}/mask",
            "action": "mask",
            "category": "privacy",
            "timeout_ms": 500,
            "fail_mode": "open",
            "phase": "outbound",
        },
    )
    assert resp.status_code == 201
    rule_id = resp.json()["id"]

    try:
        resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Tell me a secret"}],
            },
        )
        assert resp.status_code == 200, resp.text
        data = resp.json()
        content = data["choices"][0]["message"]["content"]
        assert "[WEBHOOK_MASKED]" in content
    finally:
        requests.delete(f"{base_url}/admin/v1/guardrails/{rule_id}", headers=admin_headers)
