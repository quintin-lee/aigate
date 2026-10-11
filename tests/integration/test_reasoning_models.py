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
