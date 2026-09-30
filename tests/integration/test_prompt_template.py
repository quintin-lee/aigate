#!/usr/bin/env python3
"""End-to-end integration tests for dynamic prompt template injection and middleware filter chain."""

import datetime
import os
import sys
import time
import pytest
import requests

from mock_upstream import MockUpstreamHandler


def test_model_prompt_template_injection_no_sys_msg(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-tmpl-m1"
    # Ensure model is clean
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # 1. Register model with prompt template containing variables
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
            "system_prompt": "You are an assistant for ${model} on ${date}.",
            "prompt_mode": "prepend",
        },
    )
    assert resp.status_code == 201, resp.text
    mdata = resp.json()
    assert mdata["system_prompt"] == "You are an assistant for ${model} on ${date}."
    assert mdata["prompt_mode"] == "prepend"

    # 2. Create API key without custom prompt template
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": "client-user-1",
            "allowed_models": [model_name],
            "rate_qps": 20,
            "daily_token_quota": 1000,
        },
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Client sends chat completion request without system prompt
    MockUpstreamHandler.recorded_requests.clear()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [{"role": "user", "content": "What is AI?"}],
        },
    )
    assert resp.status_code == 200, resp.text

    # 4. Check the upstream request received by MockUpstreamHandler
    reqs = [
        r for r in MockUpstreamHandler.recorded_requests
        if r.get("body", {}).get("model") == model_name
    ]
    assert len(reqs) >= 1, "Mock upstream should have received request"
    body = reqs[-1]["body"]
    messages = body.get("messages", [])
    assert len(messages) == 2, f"Expected 2 messages, got {messages}"
    assert messages[0]["role"] == "system"
    today_utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    assert messages[0]["content"] == f"You are an assistant for {model_name} on {today_utc}."
    assert messages[1]["role"] == "user"
    assert messages[1]["content"] == "What is AI?"


def test_prompt_template_modes_with_existing_system_msg(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-tmpl-m2"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    today_utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")

    # 1. Register model in PREPEND mode
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
            "system_prompt": "Company Policy ${date}.",
            "prompt_mode": "prepend",
        },
    )
    assert resp.status_code == 201, resp.text

    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={"name": "client-user-2", "allowed_models": [model_name]},
    )
    assert resp.status_code == 201, resp.text
    api_key = resp.json()["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Case A: PREPEND mode
    MockUpstreamHandler.recorded_requests.clear()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [
                {"role": "system", "content": "You are a code expert."},
                {"role": "user", "content": "Write code."},
            ],
        },
    )
    assert resp.status_code == 200, resp.text
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("body", {}).get("model") == model_name]
    assert len(reqs) >= 1
    messages = reqs[-1]["body"]["messages"]
    assert len(messages) == 2
    assert messages[0]["role"] == "system"
    assert messages[0]["content"] == f"Company Policy {today_utc}.\n\nYou are a code expert."

    # Case B: APPEND mode
    resp = requests.patch(
        f"{base_url}/admin/v1/models/{model_name}",
        headers=admin_headers,
        json={"prompt_mode": "append"},
    )
    assert resp.status_code == 200, resp.text

    MockUpstreamHandler.recorded_requests.clear()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [
                {"role": "system", "content": "You are a code expert."},
                {"role": "user", "content": "Write code."},
            ],
        },
    )
    assert resp.status_code == 200, resp.text
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("body", {}).get("model") == model_name]
    assert len(reqs) >= 1
    messages = reqs[-1]["body"]["messages"]
    assert len(messages) == 2
    assert messages[0]["role"] == "system"
    assert messages[0]["content"] == f"You are a code expert.\n\nCompany Policy {today_utc}."

    # Case C: OVERRIDE mode
    resp = requests.patch(
        f"{base_url}/admin/v1/models/{model_name}",
        headers=admin_headers,
        json={"prompt_mode": "override"},
    )
    assert resp.status_code == 200, resp.text

    MockUpstreamHandler.recorded_requests.clear()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [
                {"role": "system", "content": "You are a code expert."},
                {"role": "user", "content": "Write code."},
            ],
        },
    )
    assert resp.status_code == 200, resp.text
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("body", {}).get("model") == model_name]
    assert len(reqs) >= 1
    messages = reqs[-1]["body"]["messages"]
    assert len(messages) == 2
    assert messages[0]["role"] == "system"
    assert messages[0]["content"] == f"Company Policy {today_utc}."


def test_api_key_prompt_template_priority_over_model(gateway):
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    mock_url = gateway["mock_upstream"]

    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }

    model_name = "test-tmpl-m3"
    requests.delete(f"{base_url}/admin/v1/models/{model_name}", headers=admin_headers)

    # 1. Model has its own prompt template
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": mock_url,
            "system_prompt": "Model Default Prompt for ${model}",
            "prompt_mode": "override",
        },
    )
    assert resp.status_code == 201, resp.text

    # 2. Key configured with its own prompt template
    key_name = "vip-client-corp"
    resp = requests.post(
        f"{base_url}/admin/v1/keys",
        headers=admin_headers,
        json={
            "name": key_name,
            "allowed_models": [model_name],
            "system_prompt": "VIP Key prompt for ${key_name} on ${model}",
            "prompt_mode": "override",
        },
    )
    assert resp.status_code == 201, resp.text
    kdata = resp.json()
    assert kdata["system_prompt"] == "VIP Key prompt for ${key_name} on ${model}"
    assert kdata["prompt_mode"] == "override"
    api_key = kdata["plaintext"]

    client_headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # 3. Client request
    MockUpstreamHandler.recorded_requests.clear()
    resp = requests.post(
        f"{base_url}/v1/chat/completions",
        headers=client_headers,
        json={
            "model": model_name,
            "messages": [
                {"role": "system", "content": "User system prompt"},
                {"role": "user", "content": "Hello VIP"},
            ],
        },
    )
    assert resp.status_code == 200, resp.text
    reqs = [r for r in MockUpstreamHandler.recorded_requests if r.get("body", {}).get("model") == model_name]
    assert len(reqs) >= 1
    messages = reqs[-1]["body"]["messages"]
    assert len(messages) == 2
    assert messages[0]["role"] == "system"
    # Key-level template should have overridden Model-level and Client system prompt
    assert messages[0]["content"] == f"VIP Key prompt for {key_name} on {model_name}"


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main(["-s", "-v", __file__]))
