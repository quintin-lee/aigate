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


def test_openai_streaming(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Register model
    model_name = "test-openai-stream-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Create API key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "stream-client",
            "allowed_models": [model_name],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Stream chat completion
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "stream": True,
            "messages": [{"role": "user", "content": "tell me a joke"}],
        },
        stream=True,
    )
    assert resp.status_code == 200, resp.text
    assert "text/event-stream" in resp.headers.get("Content-Type", "")

    lines = [line for line in resp.iter_lines(decode_unicode=True) if line]
    assert any("Hello " in l for l in lines)
    assert any("from stream!" in l for l in lines)
    assert any("[DONE]" in l for l in lines)

    # 4. Verify upstream received include_usage injection
    from mock_upstream import MockUpstreamHandler
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("body", {}).get("model") == model_name]
    assert len(reqs) > 0
    upstream_body = reqs[-1]["body"]
    assert upstream_body.get("stream") is True
    assert upstream_body.get("stream_options", {}).get("include_usage") is True


def test_anthropic_non_streaming(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Register Anthropic model
    model_name = "test-claude-35-sonnet"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "anthropic",
            "endpoint": mock_url,
            "upstream_key_ref": "env:ANTHROPIC_API_KEY",
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Create API key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "claude-client",
            "allowed_models": [model_name],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Call with OpenAI request format containing multiple system messages
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [
                {"role": "system", "content": "You are a coding mentor."},
                {"role": "system", "content": "Respond with C code."},
                {"role": "user", "content": "Hello!"},
            ],
            "temperature": 0.7,
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["object"] == "chat.completion"
    assert data["model"] == model_name
    assert len(data["choices"]) > 0
    assert data["choices"][0]["message"]["content"] == "Hello from Anthropic mock!"
    assert data["usage"]["prompt_tokens"] == 14
    assert data["usage"]["completion_tokens"] == 26

    # 4. Verify Anthropic translation details received at upstream
    from mock_upstream import MockUpstreamHandler
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("path") in ("/v1/messages", "/messages")]
    assert len(reqs) > 0
    last_req = reqs[-1]
    assert "x-api-key" in last_req["headers"]
    assert "anthropic-version" in last_req["headers"]
    ubody = last_req["body"]
    assert ubody["model"] == model_name
    assert ubody["system"] == "You are a coding mentor.\n\nRespond with C code."
    assert ubody["max_tokens"] == 4096  # defaulted by provider_anthropic
    assert ubody["messages"] == [{"role": "user", "content": "Hello!"}]


def test_anthropic_streaming(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-claude-stream"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "anthropic",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "claude-stream-client",
            "allowed_models": [model_name],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Stream request from client using OpenAI format
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "stream": True,
            "messages": [{"role": "user", "content": "stream something"}],
        },
        stream=True,
    )
    assert resp.status_code == 200, resp.text
    assert "text/event-stream" in resp.headers.get("Content-Type", "")

    lines = [line for line in resp.iter_lines(decode_unicode=True) if line]
    assert any("Hello from " in l for l in lines)
    assert any("Anthropic Claude!" in l for l in lines)
    assert any("[DONE]" in l for l in lines)


def test_admin_ui_endpoints(gateway):
    base_url = gateway["base_url"]

    # 1. Root / redirects to /admin
    resp = requests.get(f"{base_url}/", allow_redirects=False)
    assert resp.status_code == 302
    assert resp.headers.get("Location") == "/admin"

    # 2. Access /admin directly
    resp = requests.get(f"{base_url}/admin")
    assert resp.status_code == 200
    assert "text/html" in resp.headers.get("Content-Type", "")
    assert "<!DOCTYPE html>" in resp.text
    assert "aigate — AI Gateway Console" in resp.text or "aigate — AI 网关控制台" in resp.text
    assert 'id="tab-overview"' in resp.text
    assert 'id="tab-models"' in resp.text
    assert 'id="tab-keys"' in resp.text
    assert 'id="tab-playground"' in resp.text

    # 3. Access /admin/ with trailing slash
    resp = requests.get(f"{base_url}/admin/")
    assert resp.status_code == 200
    assert "text/html" in resp.headers.get("Content-Type", "")


def test_gemini_chat_and_streaming(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "gemini-1.5-flash"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "gemini",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "gemini-client",
            "allowed_models": [model_name],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 1. Non-streaming call
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "hello gemini"}],
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["choices"][0]["message"]["content"] == "Hello from Gemini non-stream!"
    assert data["usage"]["prompt_tokens"] == 9
    assert data["usage"]["completion_tokens"] == 5

    # 2. Streaming call
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "stream": True,
            "messages": [{"role": "user", "content": "stream gemini"}],
        },
        stream=True,
    )
    assert resp.status_code == 200, resp.text
    assert "text/event-stream" in resp.headers.get("Content-Type", "")
    lines = [line for line in resp.iter_lines(decode_unicode=True) if line]
    assert any("Hello from Gemini " in l for l in lines)
    assert any("stream!" in l for l in lines)
    assert any("[DONE]" in l for l in lines)


def test_deepseek_reasoning_and_cache(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "deepseek-reasoner"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "deepseek",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "deepseek-client",
            "allowed_models": [model_name],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 1. Non-streaming DeepSeek with reasoning_content
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "solve math"}],
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["choices"][0]["message"]["content"] == "DeepSeek answer from mock!"
    assert data["choices"][0]["message"]["reasoning_content"] == "Thinking deeply..."
    assert data["usage"]["prompt_cache_hit_tokens"] == 15

    # 2. Streaming DeepSeek with reasoning_content
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "stream": True,
            "messages": [{"role": "user", "content": "solve math stream"}],
        },
        stream=True,
    )
    assert resp.status_code == 200, resp.text
    lines = [line for line in resp.iter_lines(decode_unicode=True) if line]
    assert any("reasoning_content" in l for l in lines)
    assert any("DeepSeek reasoning..." in l for l in lines)
    assert any("DeepSeek answer!" in l for l in lines)
    assert any("[DONE]" in l for l in lines)


def test_embeddings_dual_mode(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Register OpenAI embeddings model
    openai_emb_model = "text-embedding-3-small"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": openai_emb_model,
            "provider": "openai",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Register Gemini embeddings model
    gemini_emb_model = "text-embedding-004"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": gemini_emb_model,
            "provider": "gemini",
            "endpoint": mock_url,
        },
    )
    assert resp.status_code == 201, resp.text

    # 3. Create client key allowing both embedding models
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "emb-client",
            "allowed_models": [openai_emb_model, gemini_emb_model],
            "rate_qps": 10,
            "daily_token_quota": 5000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Mode A: OpenAI embeddings call
    resp = requests.post(
        f"{base_url}/v1/embeddings",
        headers=client_headers,
        json={
            "model": openai_emb_model,
            "input": "Embedding test with OpenAI",
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["object"] == "list"
    assert len(data["data"]) == 1
    assert data["data"][0]["object"] == "embedding"
    assert data["usage"]["prompt_tokens"] == 8

    # Mode B: Gemini embeddings call (single input)
    resp = requests.post(
        f"{base_url}/v1/embeddings",
        headers=client_headers,
        json={
            "model": gemini_emb_model,
            "input": "Embedding test with Gemini",
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["object"] == "list"
    assert len(data["data"]) == 1
    assert data["data"][0]["object"] == "embedding"
    assert data["usage"]["prompt_tokens"] == 6

    # Mode B: Gemini embeddings call (batch input)
    resp = requests.post(
        f"{base_url}/v1/embeddings",
        headers=client_headers,
        json={
            "model": gemini_emb_model,
            "input": ["item 1", "item 2"],
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data["object"] == "list"
    assert len(data["data"]) == 2
    assert data["data"][0]["index"] == 0
    assert data["data"][1]["index"] == 1
    assert data["usage"]["prompt_tokens"] == 12


def test_multi_upstream_failover_and_circuit_breaker(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Register multi-target model with priority failover:
    # Target 1 (priority 0) returns 500
    # Target 2 (priority 1) returns 200
    model_name = "failover-e2e-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "lb_policy": "priority",
            "targets": [
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/fail500",
                    "priority": 0,
                    "weight": 1,
                },
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/backup200",
                    "priority": 1,
                    "weight": 1,
                },
            ],
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Create client key
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "failover-key",
            "allowed_models": [model_name],
            "rate_qps": 50,
            "daily_token_quota": 50000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Transparent non-streaming failover test:
    # First attempt: Target 1 fails (500), gateway automatically fails over to Target 2 (200)
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "hello failover"}],
        },
    )
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert "choices" in data
    assert data["choices"][0]["message"]["content"] == "Hello from mock upstream!"

    # 4. Verify Prometheus metrics recorded failover
    resp = requests.get(f"{base_url}/metrics")
    assert resp.status_code == 200
    assert "aigate_failover_total" in resp.text
    assert f'model="{model_name}"' in resp.text

    # 5. Fire 2 more requests to reach 3 consecutive failures for Target 1 -> trip Circuit Breaker to OPEN
    for _ in range(2):
        r = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={"model": model_name, "messages": [{"role": "user", "content": "ping"}]},
        )
        assert r.status_code == 200

    # Verify model list shows cb_state="open" for target 1 and "closed" for target 2
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200
    models_data = resp.json()
    models_list = models_data.get("models") or models_data.get("data") or []
    m_info = next((m for m in models_list if (m.get("name") == model_name or m.get("model_name") == model_name)), None)
    assert m_info is not None
    assert len(m_info["targets"]) == 2
    assert m_info["targets"][0]["cb_state"] == "open"
    assert m_info["targets"][1]["cb_state"] == "closed"

    # 6. Streaming failover test
    stream_model = "failover-stream-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": stream_model,
            "lb_policy": "priority",
            "targets": [
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/fail429",
                    "priority": 0,
                    "weight": 1,
                },
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/backup_stream",
                    "priority": 1,
                    "weight": 1,
                },
            ],
        },
    )
    assert resp.status_code == 201

    # Update key to allow streaming model
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "stream-key",
            "allowed_models": [stream_model],
            "rate_qps": 50,
            "daily_token_quota": 50000,
        },
    )
    assert resp.status_code == 201
    stream_api_key = resp.json()["plaintext"]

    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers={"Authorization": f"Bearer {stream_api_key}", "Content-Type": "application/json"},
        json={
            "model": stream_model,
            "messages": [{"role": "user", "content": "stream failover"}],
            "stream": True,
        },
        stream=True,
    )
    assert resp.status_code == 200
    assert "text/event-stream" in resp.headers.get("Content-Type", "")
    chunks = [line.decode("utf-8") for line in resp.iter_lines() if line]
    assert any("data: " in c for c in chunks)
    assert any("[DONE]" in c for c in chunks)


def test_multi_upstream_load_balancing(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # Register model with weighted_round_robin policy
    wrr_model = "wrr-e2e-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": wrr_model,
            "lb_policy": "weighted_round_robin",
            "targets": [
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/wrr_target_1",
                    "priority": 0,
                    "weight": 3,
                },
                {
                    "provider": "openai",
                    "endpoint": f"{mock_url}/wrr_target_2",
                    "priority": 0,
                    "weight": 1,
                },
            ],
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "wrr-key",
            "allowed_models": [wrr_model],
            "rate_qps": 100,
            "daily_token_quota": 50000,
        },
    )
    assert resp.status_code == 201
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Send 10 requests, verify all succeed
    for _ in range(10):
        r = requests.post(
            f"{base_url}/v1/chat/completions",
            headers=client_headers,
            json={"model": wrr_model, "messages": [{"role": "user", "content": "wrr test"}]},
        )
        assert r.status_code == 200, r.text


def test_provider_management_and_multi_model_routing(gateway):
    """Test provider CRUD, auto-sync of multiple models, direct API key resolution, and completions."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Create a provider with 2 models and a direct raw API key
    provider_name = "e2e-provider-deepseek"
    raw_api_key = "sk-deepseek-direct-key-987654"
    models_list = ["ds-chat-v3", "ds-reasoner-r1"]

    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": provider_name,
            "provider_type": "deepseek",
            "endpoint": mock_url,
            "api_key": raw_api_key,
            "models": models_list,
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    p_data = resp.json()
    assert p_data["created"] is True
    provider_id = p_data["id"]
    assert provider_id > 0

    # 2. List providers and verify masked API key & models
    resp = requests.get(f"{base_url}/admin/v1/providers", headers=admin_headers)
    assert resp.status_code == 200
    providers = resp.json().get("providers", [])
    matched = [p for p in providers if p["id"] == provider_id]
    assert len(matched) == 1
    prov = matched[0]
    assert prov["name"] == provider_name
    assert prov["endpoint"] == mock_url
    assert prov["models"] == models_list
    assert prov["enabled"] == 1 or prov["enabled"] is True
    # Masked API key display
    assert "••••" in prov["api_key"]
    assert prov["api_key"].endswith("7654")

    # 3. Verify auto-synced models in /admin/v1/models
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200
    all_models = {m["name"]: m for m in resp.json().get("models", [])}
    assert "ds-chat-v3" in all_models
    assert "ds-reasoner-r1" in all_models
    assert all_models["ds-chat-v3"]["endpoint"] == mock_url

    # 4. Create client API key with access to ds-chat-v3
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "provider-client-key",
            "allowed_models": ["ds-chat-v3"],
            "rate_qps": 50,
            "daily_token_quota": 50000,
        },
    )
    assert resp.status_code == 201
    client_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {client_key}",
        "Content-Type": "application/json",
    }

    # 5. Successful chat completions using auto-synced model with provider's direct key
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": "ds-chat-v3",
            "messages": [{"role": "user", "content": "hello deepseek"}],
        },
    )
    assert resp.status_code == 200, resp.text
    chat_res = resp.json()
    assert "choices" in chat_res
    assert chat_res["choices"][0]["message"]["content"] == "Hello from mock upstream!"

    # 6. Update provider (PATCH)
    resp = requests.patch(
        f"{base_url}/admin/v1/providers/{provider_id}",
        headers=admin_headers,
        json={
            "endpoint": f"{mock_url}/v2",
            "enabled": False,
        },
    )
    assert resp.status_code == 200, resp.text

    # 7. Delete provider
    resp = requests.delete(f"{base_url}/admin/v1/providers/{provider_id}", headers=admin_headers)
    assert resp.status_code == 200, resp.text

    # Verify provider list no longer contains this provider
    resp = requests.get(f"{base_url}/admin/v1/providers", headers=admin_headers)
    assert resp.status_code == 200
    providers = resp.json().get("providers", [])
    assert not any(p["id"] == provider_id for p in providers)

def test_body_size_cap_413(gateway):
    """Bodies above AIGATE_MAX_BODY_BYTES are rejected with 413 before processing."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-413-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={"name": model_name, "provider": "openai", "endpoint": mock_url},
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={"name": "cap-client", "allowed_models": [model_name]},
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]
    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Under the 2048-byte fixture cap: accepted (200 from mock upstream)
    small = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "hi"}]},
    )
    assert small.status_code == 200, small.text

    # 4 KiB of content far exceeds the cap -> 413, JSON error, body untouched
    big = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "x" * 4096}],
        },
    )
    assert big.status_code == 413, big.text
    err = big.json()["error"]
    assert err["type"] == "payload_too_large"
    assert err["code"] == 413

def test_upstream_400_passthrough(gateway):
    """A non-failover upstream 400 is passed through to the client verbatim."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-400-passthrough-model"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": f"{mock_url}/fail400",
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={"name": "passthrough-key", "allowed_models": [model_name]},
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]
    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "hi"}]},
    )
    # Upstream 400 body is forwarded verbatim instead of a generic 502
    assert resp.status_code == 400, resp.text
    assert resp.json()["error"] == "simulated failure 400"


def test_provider_probe_endpoint(gateway):
    """P1-4: POST /admin/v1/providers/{id}/test probes a provider's /models endpoint.

    Must run BEFORE test_admin_lockout_429 (which locks this client IP)."""
    import uuid
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]
    suffix = uuid.uuid4().hex[:8]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Reachable mock endpoint -> verdict ok
    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": f"probe-ok-provider-{suffix}",
            "provider_type": "openai",
            "endpoint": mock_url,
            "api_key": "sk-probe-direct",
            "models": [f"probe-model-{suffix}"],
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    provider_id = resp.json()["id"]

    resp = requests.post(f"{base_url}/admin/v1/providers/{provider_id}/test",
                         headers=admin_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["verdict"] == "ok", body
    assert body["status"] == 200
    assert body["provider"] == provider_id

    # 2. Unreachable endpoint (closed port) -> verdict unreachable
    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": f"probe-dead-provider-{suffix}",
            "provider_type": "openai",
            "endpoint": "http://127.0.0.1:9",
            "api_key": "sk-probe-dead",
            "models": [f"probe-dead-model-{suffix}"],
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    dead_id = resp.json()["id"]

    resp = requests.post(f"{base_url}/admin/v1/providers/{dead_id}/test",
                         headers=admin_headers)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["verdict"] == "unreachable", body
    assert body["status"] == 0


def test_provider_key_encrypted_storage(gateway, pg_dsn):
    """Direct plaintext provider keys are stored encrypted (pg: prefix) when
    AIGATE_MASTER_KEY is set; env: references pass through untouched."""
    import subprocess

    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]
    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    def stored_key(provider_id: int) -> str:
        res = subprocess.run(
            ["psql", "-A", "-t", pg_dsn, "-c",
             f"SELECT api_key FROM providers WHERE id = {provider_id}"],
            capture_output=True, text=True,
        )
        assert res.returncode == 0, res.stderr
        return res.stdout.strip()

    # 1. Direct plaintext key -> stored as pg:<encrypted>
    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": "key-enc-plaintext",
            "provider_type": "openai",
            "endpoint": mock_url,
            "api_key": "sk-plaintext-secret-do-not-store-raw",
            "models": ["enc-model"],
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    plain_id = resp.json()["id"]
    stored = stored_key(plain_id)
    assert stored.startswith("pg:"), f"expected pg:-prefixed storage, got {stored!r}"
    assert "sk-plaintext-secret-do-not-store-raw" not in stored

    # 2. env: reference -> stored verbatim (resolved at request time)
    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": "key-enc-envref",
            "provider_type": "anthropic",
            "endpoint": mock_url,
            "api_key": "env:ANTHROPIC_API_KEY",
            "models": ["enc-env-model"],
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    env_id = resp.json()["id"]
    assert stored_key(env_id) == "env:ANTHROPIC_API_KEY"

    # 3. Update with another plaintext key -> re-encrypted
    resp = requests.patch(
        f"{base_url}/admin/v1/providers/{plain_id}",
        headers=admin_headers,
        json={"api_key": "sk-rotated-key-5678"},
    )
    assert resp.status_code == 200, resp.text
    assert stored_key(plain_id).startswith("pg:")

    # cleanup: drop both providers (their auto-synced models cascade)
    for pid in (plain_id, env_id):
        resp = requests.delete(
            f"{base_url}/admin/v1/providers/{pid}", headers=admin_headers)
        assert resp.status_code == 200, resp.text


def test_pg_reconnect_recovers(gateway, pg_dsn):
    """Kill the gateway's PG backend mid-session; store ops then recover via
    the reconnect path (pg_store.c pq_ensure_conn) once libpq's lazy
    detection flips PQstatus to CONNECTION_BAD after the first failed query.

    libpq does not probe the socket proactively: the first op on the dead
    connection fails, and the NEXT op (flush worker or any admin read)
    triggers the reconnect. So we poll an admin key-list read -- which is
    always a PG round-trip -- until it stops returning 500.
    """
    import subprocess

    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]
    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # Fresh provider/key so this test is self-contained.
    resp = requests.post(
        f"{base_url}/admin/v1/providers",
        headers=admin_headers,
        json={
            "name": "reconnect-provider",
            "provider_type": "openai",
            "endpoint": mock_url,
            "api_key": "env:ANTHROPIC_API_KEY",
            "models": ["reconnect-model"],
            "enabled": True,
        },
    )
    assert resp.status_code == 201, resp.text
    provider_id = resp.json()["id"]

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={"name": "reconnect-client", "allowed_models": ["reconnect-model"]},
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]
    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Baseline: request works before the kill.
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": "reconnect-model",
              "messages": [{"role": "user", "content": "hi"}]},
    )
    assert resp.status_code == 200, resp.text

    # Terminate every PG backend on the aigate DB that is not this psql
    # session -> the gateway's single libpq connection dies (CONNECTION_BAD).
    res = subprocess.run(
        ["psql", "-A", "-t", pg_dsn, "-c",
         "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
         "WHERE datname = current_database() AND pid <> pg_backend_pid() "
         "AND usename = 'aigate'"],
        capture_output=True, text=True,
    )
    assert res.returncode == 0, res.stderr
    assert "t" in res.stdout, f"expected at least one terminated backend: {res.stdout!r}"

    # Poll until the store recovers: first PG op after the kill fails
    # (lazy detection), the next op reconnects. The admin key list always
    # hits PG, so it is the observable recovery signal (500 until then).
    import time
    deadline = time.time() + 30
    last = None
    recovered = False
    while time.time() < deadline:
        last = requests.get(f"{base_url}/admin/v1/keys", headers=admin_headers)
        if last.status_code == 200:
            recovered = True
            break
        time.sleep(0.5)
    assert recovered, f"PG reconnect did not recover within 30s; last: {last.status_code if last is not None else None}"

    # End-to-end: chat still works after recovery.
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": "reconnect-model",
              "messages": [{"role": "user", "content": "hi again"}]},
    )
    assert resp.status_code == 200, resp.text
    assert resp.json()["choices"][0]["message"]["content"] == "Hello from mock upstream!"

    # cleanup: delete the provider (its auto-synced models cascade)
    resp = requests.delete(
        f"{base_url}/admin/v1/providers/{provider_id}", headers=admin_headers)
    assert resp.status_code == 200, resp.text


def test_groups_and_cost_attribution(gateway):
    """P1-5: Groups CRUD, pricing on models, and cost attribution aggregation."""
    import time
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]
    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    import uuid
    group_name = f"rnd-dept-{uuid.uuid4().hex[:8]}"
    resp = requests.post(
        f"{base_url}/admin/v1/groups",
        headers=admin_headers,
        json={"name": group_name},
    )
    assert resp.status_code == 201, resp.text
    group_data = resp.json()
    group_id = group_data["id"]
    assert group_id > 0
    assert group_data["name"] == group_name

    # Duplicate group name returns 409
    dup = requests.post(
        f"{base_url}/admin/v1/groups",
        headers=admin_headers,
        json={"name": group_name},
    )
    assert dup.status_code == 409, dup.text
    assert dup.json()["error"]["type"] == "group_exists"

    # 2. List groups
    resp = requests.get(f"{base_url}/admin/v1/groups", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    groups = resp.json().get("groups", [])
    matched_group = next((g for g in groups if g["id"] == group_id), None)
    assert matched_group is not None
    assert matched_group["name"] == group_name
    assert matched_group["key_count"] == 0

    # 3. Create model with pricing
    model_name = f"priced-model-{uuid.uuid4().hex[:8]}"
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
            "pricing": {
                "in_mtok": 1000.0,
                "out_mtok": 3000.0,
                "cached_mtok_discount": 0.5,
            },
        },
    )
    assert resp.status_code == 201, resp.text

    # Verify model has pricing in GET /admin/v1/models
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    models = resp.json().get("models", [])
    m_info = next((m for m in models if m.get("name") == model_name), None)
    assert m_info is not None
    assert m_info.get("pricing", {}).get("in_mtok") == 1000.0

    # 4. Create API key assigned to group
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "rnd-client-key",
            "allowed_models": [model_name],
            "group_id": group_id,
        },
    )
    assert resp.status_code == 201, resp.text
    key_info = resp.json()
    api_key = key_info["plaintext"]
    key_id = key_info["key_id"]

    # Verify key list contains group_id
    resp = requests.get(f"{base_url}/admin/v1/keys", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    keys_list = resp.json().get("keys", [])
    k_found = next((k for k in keys_list if k["key_id"] == key_id), None)
    assert k_found is not None
    assert k_found.get("group_id") == group_id

    # Verify group key_count is now 1
    resp = requests.get(f"{base_url}/admin/v1/groups", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    matched_group = next((g for g in resp.json().get("groups", []) if g["id"] == group_id), None)
    assert matched_group is not None
    assert matched_group["key_count"] == 1

    # 5. Deleting group with attached keys fails with 409
    del_resp = requests.delete(f"{base_url}/admin/v1/groups/{group_id}", headers=admin_headers)
    assert del_resp.status_code == 409, del_resp.text
    assert del_resp.json()["error"]["type"] == "group_has_keys"

    # 6. Send chat completion request
    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={"model": model_name, "messages": [{"role": "user", "content": "cost attribution test"}]},
    )
    assert resp.status_code == 200, resp.text
    usage = resp.json().get("usage", {})
    p_tok = usage.get("prompt_tokens", 7)
    c_tok = usage.get("completion_tokens", 11)

    # 7. Poll cost attribution endpoint until the row is flushed by the worker
    deadline = time.time() + 10
    cost_rows = []
    while time.time() < deadline:
        c_resp = requests.get(f"{base_url}/admin/v1/cost?group={group_id}&by=model", headers=admin_headers)
        if c_resp.status_code == 200:
            data = c_resp.json()
            cost_rows = data.get("rows", [])
            if cost_rows:
                break
        time.sleep(0.5)

    assert len(cost_rows) >= 1, f"Cost report did not return rows in time: {cost_rows}"
    crow = cost_rows[0]
    assert crow["model"] == model_name
    assert crow["prompt_tokens"] >= p_tok
    assert crow["completion_tokens"] >= c_tok
    # With in_mtok=1000, out_mtok=3000, prompt=7, completion=11: (7*1000 + 11*3000)/1e6 = 0.04 -> 4 cents
    assert crow.get("cost_cents") is not None
    assert crow["cost_cents"] >= 4

    # 8. Clean up key and group: patch key group_id to null, then delete group
    patch_resp = requests.patch(
        f"{base_url}/admin/v1/keys/{key_id}",
        headers=admin_headers,
        json={"group_id": None},
    )
    assert patch_resp.status_code == 200, patch_resp.text

    del_resp = requests.delete(f"{base_url}/admin/v1/groups/{group_id}", headers=admin_headers)
    assert del_resp.status_code == 200, del_resp.text


def test_list_pagination(gateway):
    """Test pagination across all admin collection endpoints (P0-P2 requirement)."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    # 1. Models pagination
    r = requests.get(f"{base_url}/admin/v1/models?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2
    assert len(data.get("models", [])) <= 2

    # 2. Keys pagination
    r = requests.get(f"{base_url}/admin/v1/keys?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2
    assert len(data.get("keys", [])) <= 2

    # 3. Providers pagination
    r = requests.get(f"{base_url}/admin/v1/providers?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2
    assert len(data.get("providers", [])) <= 2

    # 4. Groups pagination
    r = requests.get(f"{base_url}/admin/v1/groups?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2
    assert len(data.get("groups", [])) <= 2

    # 5. Usage & Usage Requests pagination
    r = requests.get(f"{base_url}/admin/v1/usage?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2

    r = requests.get(f"{base_url}/admin/v1/usage/requests?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2

    # 6. Cost pagination
    r = requests.get(f"{base_url}/admin/v1/cost?page=1&limit=2", headers=admin_headers)
    assert r.status_code == 200, r.text
    data = r.json()
    assert "total" in data
    assert data["page"] == 1
    assert data["limit"] == 2


def test_admin_lockout_429(gateway):
    """10 failed admin auth attempts from one IP lock that IP out (429).

    Runs last: the 300s lockout window blocks this client IP from further
    admin calls, including correct tokens (by design)."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]

    for _ in range(10):
        r = requests.get(
            f"{base_url}/admin/v1/keys",
            headers={"Authorization": "Bearer definitely-wrong-token"},
        )
        assert r.status_code == 401, r.text

    r = requests.get(
        f"{base_url}/admin/v1/keys",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    assert r.status_code == 429, r.text
    err = r.json()["error"]
    assert err["type"] == "locked_out"




