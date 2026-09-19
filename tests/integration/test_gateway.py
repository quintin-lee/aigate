"""End-to-end integration tests for aigate (spec §6, Task 12)."""

import time
import requests

def test_full_lifecycle(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Register model pointing to mock upstream
    model_name = "test-gpt-4o"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
            "default_params": {"temperature": 0.5},
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Create client API key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "integration-client",
            "allowed_models": [model_name],
            "rate_qps": 5,
            "daily_token_quota": 50,
        },
    )
    assert resp.status_code == 201, resp.text
    key_data = resp.json()
    key_id = key_data["key_id"]
    api_key = key_data["plaintext"]
    assert api_key.startswith("aig_")

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Successful chat completion
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "hi"}],
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert "choices" in data
    assert data["choices"][0]["message"]["content"] == "Hello from mock upstream!"
    assert data["usage"]["prompt_tokens"] == 7
    assert data["usage"]["completion_tokens"] == 11

    # 4. Burst QPS limit (fire requests quickly with rate_qps=5)
    rate_limited = False
    for _ in range(15):
        r = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={"model": model_name, "messages": [{"role": "user", "content": "hi"}]},
        )
        if r.status_code == 429:
            assert "Retry-After" in r.headers
            rate_limited = True
            break
    assert rate_limited, "Expected 429 on rapid burst exceeding rate_qps"

    # Wait for token bucket to refill
    time.sleep(1.5)

    # 5. Revoke key -> returns 401
    resp = requests.delete(f"{base_url}/admin/v1/keys/{key_id}", headers=admin_headers)
    assert resp.status_code == 200

    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "hi"}]},
    )
    assert resp.status_code == 401

    # 6. Verify Prometheus metrics
    resp = requests.get(f"{base_url}/metrics")
    assert resp.status_code == 200
    assert "aigate_requests_total" in resp.text
