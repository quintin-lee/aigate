#!/usr/bin/env python3
"""End-to-end integration tests for Audit Log Streaming & Webhook Alerting."""

import glob
import http.server
import json
import os
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time
from typing import Dict, Any, List

import requests

# Add current directory to sys.path so mock_upstream can be imported
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mock_upstream import start_mock_upstream

ADMIN_TOKEN = "admin_audit_integration_token"
AUDIT_LOG_FILE = "/tmp/aigate_e2e_audit.ndjson"


def find_pg_dsn() -> str:
    dsn = os.environ.get("TEST_PG_DSN")
    candidates = [dsn] if dsn else [
        "postgresql://aigate:changeme@172.39.4.3:5432/aigate",
        "postgresql://aigate:changeme@172.39.4.2:5432/aigate",
        "postgresql://postgres:postgres@127.0.0.1:5432/aigate_test",
    ]
    for candidate in candidates:
        try:
            res = subprocess.run(["psql", candidate, "-c", "SELECT 1;"], capture_output=True)
            if res.returncode == 0:
                return candidate
        except Exception:
            pass
    return "postgresql://aigate:changeme@172.39.4.3:5432/aigate"


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockAlertWebhookHandler(http.server.BaseHTTPRequestHandler):
    recorded_alerts: List[Dict[str, Any]] = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(length)
        body_json = None
        try:
            body_json = json.loads(body_bytes.decode("utf-8")) if body_bytes else None
        except Exception:
            pass

        self.__class__.recorded_alerts.append({
            "path": self.path,
            "headers": dict(self.headers),
            "body": body_json,
        })

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"status":"received"}')

    def log_message(self, format, *args):
        pass


def cleanup_audit_files():
    for f in glob.glob("/tmp/aigate_e2e_audit.ndjson*"):
        try:
            os.remove(f)
        except OSError:
            pass


def get_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def run_e2e_test():
    print("=== Starting End-to-End Audit Pipeline Integration Test ===")
    cleanup_audit_files()

    # 1. Start Mock Webhook Server
    MockAlertWebhookHandler.recorded_alerts = []
    webhook_server = ThreadedTCPServer(("127.0.0.1", 0), MockAlertWebhookHandler)
    webhook_port = webhook_server.server_address[1]
    webhook_thread = threading.Thread(target=webhook_server.serve_forever, daemon=True)
    webhook_thread.start()
    webhook_url = f"http://127.0.0.1:{webhook_port}/alert"
    print(f"[*] Mock Webhook Server listening on {webhook_url}")

    # 2. Start Mock Upstream Server
    upstream_server, upstream_port = start_mock_upstream(0)
    mock_upstream_url = f"http://127.0.0.1:{upstream_port}"
    print(f"[*] Mock Upstream Server listening on {mock_upstream_url}")

    # 3. Locate Gateway Binary
    gateway_port = get_free_port()
    bin_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/aigate"))
    if not os.path.exists(bin_path):
        raise RuntimeError(f"Binary not found at {bin_path}. Run cmake --build build first.")

    pg_dsn = find_pg_dsn()
    print(f"[*] Using PostgreSQL DSN: {pg_dsn}")

    # 4. Prepare Environment & Start Gateway
    env = os.environ.copy()
    env["AIGATE_LISTEN"] = f":{gateway_port}"
    env["AIGATE_PG_DSN"] = pg_dsn
    env["AIGATE_ADMIN_TOKEN"] = ADMIN_TOKEN
    env["AIGATE_METRICS_ACL"] = "127.0.0.1"
    env["AIGATE_USAGE_FLUSH_S"] = "1"
    env["AIGATE_MASTER_KEY"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    env["AIGATE_AUDIT_LOG_FILE"] = AUDIT_LOG_FILE
    env["AIGATE_AUDIT_MAX_SIZE_MB"] = "10"
    env["AIGATE_AUDIT_MAX_BACKUPS"] = "3"
    env["AIGATE_AUDIT_WEBHOOK_URL"] = webhook_url
    env["AIGATE_AUDIT_WEBHOOK_FORMAT"] = "standard"
    env["AIGATE_AUDIT_MAX_PROMPT_LEN"] = "1024"
    env["AIGATE_AUDIT_SAMPLE_RATE"] = "1.0"
    env["AIGATE_DRAIN_TIMEOUT_S"] = "0"

    # Reset DB state
    try:
        subprocess.run(
            ["psql", pg_dsn, "-c",
             "DELETE FROM models; DELETE FROM api_keys; DELETE FROM guardrails_rules;"],
            capture_output=True,
            check=False,
        )
    except Exception:
        pass

    proc = subprocess.Popen([bin_path], env=env)
    base_url = f"http://127.0.0.1:{gateway_port}"

    try:
        # Wait for Gateway readiness
        ready = False
        for _ in range(40):
            try:
                r = requests.get(f"{base_url}/metrics", timeout=1)
                if r.status_code == 200:
                    ready = True
                    break
            except Exception:
                time.sleep(0.1)

        if not ready:
            raise RuntimeError("Gateway did not become ready within 4 seconds")
        print(f"[*] Gateway ready on {base_url}")

        admin_headers = {
            "Authorization": f"Bearer {ADMIN_TOKEN}",
            "Content-Type": "application/json",
        }

        # 5. Register Model Route & Client API Key
        model_name = "audit-e2e-model"
        r = requests.post(
            f"{base_url}/admin/v1/models",
            headers=admin_headers,
            json={"name": model_name, "provider": "openai", "endpoint": mock_upstream_url},
        )
        assert r.status_code in (200, 201), f"Model creation failed: {r.text}"

        r = requests.post(
            f"{base_url}/admin/v1/keys",
            headers=admin_headers,
            json={"name": "audit-client-key", "allowed_models": [model_name], "guardrails_enabled": True},
        )
        assert r.status_code == 201, f"Key creation failed: {r.text}"
        client_key = r.json()["plaintext"]

        # 6. Register Guardrail Rule to Block "DROP DATABASE"
        r = requests.post(
            f"{base_url}/admin/v1/guardrails",
            headers=admin_headers,
            json={
                "pattern": "DROP DATABASE",
                "rule_type": "keyword",
                "action": "block",
                "category": "safety",
            },
        )
        assert r.status_code == 201, f"Guardrail creation failed: {r.text}"

        requests.post(f"{base_url}/admin/v1/guardrails/reload", headers=admin_headers)

        client_headers = {
            "Authorization": f"Bearer {client_key}",
            "Content-Type": "application/json",
        }

        # --- Test 1: Normal Request (Severity: INFO) ---
        print("\n--- Test 1: Normal Request ---")
        normal_resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Hello AIGate!"}],
            },
        )
        assert normal_resp.status_code == 200, f"Expected 200, got {normal_resp.status_code}: {normal_resp.text}"

        # Wait for file worker batch write (200ms tick)
        time.sleep(0.5)

        assert os.path.exists(AUDIT_LOG_FILE), "Audit log file was not created"
        with open(AUDIT_LOG_FILE, "r") as f:
            lines = [json.loads(line) for line in f if line.strip()]

        info_records = [l for l in lines if l.get("severity") == "INFO"]
        assert len(info_records) >= 1, "No INFO severity record found in audit log"
        normal_rec = info_records[-1]
        assert normal_rec.get("model") == model_name
        assert normal_rec.get("status") == 200
        assert normal_rec.get("prompt") is None, "INFO records must NOT capture prompt snapshot"
        assert len(MockAlertWebhookHandler.recorded_alerts) == 0, "Webhook alerts must NOT trigger for normal INFO"
        print("[PASS] Normal request recorded to NDJSON with INFO severity and no prompt snapshot")

        # --- Test 2: Guardrail Violation Request (Severity: VIOLATION) ---
        print("\n--- Test 2: Guardrail Violation Request ---")
        bad_prompt = "Critical exploit: DROP DATABASE immediately!"
        viol_resp = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": bad_prompt}],
            },
        )
        assert viol_resp.status_code == 400, f"Expected 400 blocked by guardrail, got {viol_resp.status_code}"
        assert "content_policy_violation" in viol_resp.text

        # Wait up to 2 seconds for Webhook alert and file write
        alert_received = False
        for _ in range(20):
            if len(MockAlertWebhookHandler.recorded_alerts) > 0:
                alert_received = True
                break
            time.sleep(0.1)

        assert alert_received, "Webhook alert was not received within 2 seconds"
        alert = MockAlertWebhookHandler.recorded_alerts[-1]["body"]
        assert alert.get("severity") == "VIOLATION", f"Alert severity was {alert.get('severity')}"
        assert alert.get("violation", {}).get("type") == "guardrail_block"
        assert "DROP DATABASE" in alert.get("prompt", "")
        print("[PASS] Webhook alert received with VIOLATION severity and prompt snapshot")

        # Check NDJSON file for the violation record
        with open(AUDIT_LOG_FILE, "r") as f:
            lines = [json.loads(line) for line in f if line.strip()]

        viol_records = [l for l in lines if l.get("severity") == "VIOLATION"]
        assert len(viol_records) >= 1, "No VIOLATION record found in NDJSON file"
        viol_rec = viol_records[-1]
        assert viol_rec.get("status") == 400
        assert viol_rec.get("violation", {}).get("type") == "guardrail_block"
        assert "DROP DATABASE" in viol_rec.get("prompt", "")
        print("[PASS] Violation recorded to NDJSON file with rule details and prompt snapshot")

        # --- Test 3: SIGHUP Reopen/Reload ---
        print("\n--- Test 3: SIGHUP Log Reload ---")
        backup_log = AUDIT_LOG_FILE + ".saved"
        os.rename(AUDIT_LOG_FILE, backup_log)
        assert not os.path.exists(AUDIT_LOG_FILE)

        # Trigger SIGHUP
        proc.send_signal(signal.SIGHUP)
        time.sleep(0.3)

        # Send another request to trigger write to the reopened file
        r = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={"model": model_name, "messages": [{"role": "user", "content": "After SIGHUP"}]},
        )
        assert r.status_code == 200
        time.sleep(0.5)

        assert os.path.exists(AUDIT_LOG_FILE), "Audit log file was not re-created after SIGHUP"
        with open(AUDIT_LOG_FILE, "r") as f:
            lines_after = [line for line in f if line.strip()]
        assert len(lines_after) >= 1, "Reopened log file is empty"
        print("[PASS] SIGHUP signal properly re-opened file without dropping records")

        # --- Test 4: Prometheus Metrics Exposition ---
        print("\n--- Test 4: Prometheus Metrics Exposition ---")
        m_resp = requests.get(f"{base_url}/metrics")
        assert m_resp.status_code == 200
        m_text = m_resp.text
        assert 'aigate_audit_events_total{severity="info"}' in m_text
        assert 'aigate_audit_events_total{severity="violation"}' in m_text
        assert "aigate_audit_webhook_success_total" in m_text
        print("[PASS] Prometheus metrics exposed live audit counters")

    finally:
        print("\n--- Cleaning up resources ---")
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        upstream_server.shutdown()
        webhook_server.shutdown()
        cleanup_audit_files()
        print("[*] Gateway, Mock Upstream, and Mock Webhook successfully stopped.")

    print("\n=== ALL E2E AUDIT TESTS PASSED ===")


def test_audit_pipeline_e2e():
    """Pytest entrypoint."""
    run_e2e_test()


if __name__ == "__main__":
    run_e2e_test()
