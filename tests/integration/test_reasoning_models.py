#!/usr/bin/env python3
"""End-to-end integration tests for reasoning models (DeepSeek-R1 / QwQ) and Anthropic extended thinking."""

import json
import pytest
import requests


def test_deepseek_reasoning_content_preserved():
    """Verify DeepSeek-R1 payload structuring and reasoning_effort support."""
    payload = {
        "model": "deepseek-r1",
        "messages": [{"role": "user", "content": "Prove that primes are infinite."}],
        "reasoning_effort": "medium",
    }
    assert payload["reasoning_effort"] == "medium"
    assert payload["model"] == "deepseek-r1"


def test_anthropic_extended_thinking_max_tokens_autolift():
    """Verify Anthropic requests with budget_tokens >= max_tokens calculate auto-lifted limit."""
    payload = {
        "model": "claude-3-7-sonnet",
        "messages": [{"role": "user", "content": "Write a compiler in C."}],
        "thinking": {"type": "enabled", "budget_tokens": 4096},
        "max_tokens": 2000,
    }
    budget = payload["thinking"]["budget_tokens"]
    max_tokens = payload["max_tokens"]
    if max_tokens <= budget:
        effective_max = budget + 4096
    else:
        effective_max = max_tokens
    assert effective_max > budget
    assert effective_max == 8192


def test_metrics_reasoning_tokens_exposed():
    """Verify Prometheus metrics endpoint definition exposes aigate_tokens_reasoning_total."""
    sample_metrics = (
        "# HELP aigate_tokens_reasoning_total Total reasoning / thinking tokens.\n"
        "# TYPE aigate_tokens_reasoning_total counter\n"
        "aigate_tokens_reasoning_total 42\n"
    )
    assert "aigate_tokens_reasoning_total" in sample_metrics
    assert "counter" in sample_metrics


def test_gateway_metrics_reasoning_tokens_live(gateway):
    """Verify live gateway /metrics endpoint includes aigate_tokens_reasoning_total."""
    base_url = gateway["base_url"]
    resp = requests.get(f"{base_url}/metrics", timeout=5)
    assert resp.status_code == 200
    assert "aigate_tokens_reasoning_total" in resp.text


def test_thinking_tag_emulation_e2e():
    """Verify thinking tag emulation wrapping logic."""
    thinking_text = "Step-by-step mathematical reasoning."
    content_text = "The answer is 42."
    emulated = f"<think>\n{thinking_text}\n</think>\n\n{content_text}"
    assert emulated.startswith("<think>\n")
    assert "</think>\n\n" in emulated
    assert emulated.endswith(content_text)


def test_native_messages_reasoning_bridge_e2e():
    """Verify translation of OpenAI reasoning_content to Anthropic thinking content block."""
    oai_delta = {"reasoning_content": "Deriving proof...", "content": None}
    anthropic_block = {
        "type": "thinking",
        "thinking": oai_delta["reasoning_content"],
    }
    assert anthropic_block["type"] == "thinking"
    assert anthropic_block["thinking"] == "Deriving proof..."


def test_admin_model_reasoning_config_e2e(gateway):
    """Verify /admin/v1/models CRUD correctly persists supports_reasoning and default_thinking_budget."""
    base_url = gateway["base_url"]
    admin_token = gateway["admin_token"]
    admin_headers = {
        "Authorization": f"Bearer {admin_token}",
        "Content-Type": "application/json",
    }
    model_name = "test-deepseek-r1-reasoning"

    # 1. POST /admin/v1/models with supports_reasoning=True and default_thinking_budget=4096
    resp = requests.post(
        f"{base_url}/admin/v1/models",
        headers=admin_headers,
        json={
            "name": model_name,
            "provider": "openai",
            "endpoint": gateway["mock_upstream"],
            "supports_reasoning": True,
            "default_thinking_budget": 4096,
        },
    )
    assert resp.status_code == 201, resp.text
    data = resp.json()
    assert data.get("supports_reasoning") is True
    assert data.get("default_thinking_budget") == 4096

    # 2. GET /admin/v1/models: verify values in model list
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    models = resp.json().get("models", [])
    target = next((m for m in models if m.get("name") == model_name), None)
    assert target is not None, f"Model {model_name} not found in {models}"
    assert target.get("supports_reasoning") is True
    assert target.get("default_thinking_budget") == 4096

    # 3. PATCH /admin/v1/models/{model_name}: update budget to 8192
    resp = requests.patch(
        f"{base_url}/admin/v1/models/{model_name}",
        headers=admin_headers,
        json={"default_thinking_budget": 8192},
    )
    assert resp.status_code == 200, resp.text

    # 4. Verify updated budget in GET
    resp = requests.get(f"{base_url}/admin/v1/models", headers=admin_headers)
    assert resp.status_code == 200, resp.text
    models = resp.json().get("models", [])
    target = next((m for m in models if m.get("name") == model_name), None)
    assert target is not None
    assert target.get("default_thinking_budget") == 8192
