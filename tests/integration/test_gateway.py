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
    assert "aigate — AI Gateway Console" in resp.text
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


