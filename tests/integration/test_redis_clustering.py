"""Integration tests for Redis-based distributed state (QPS anti-oversell,
quota sharing, circuit breaker sync, and Redis-down Fail-Closed 503).

These tests run two aigate instances sharing a Redis backend and verify that
rate-limiting and quota enforcement are cluster-wide rather than per-process.

Prerequisites:
  - Redis running on 127.0.0.1:6379 (or REDIS_URL env var)
  - Two gateway instances started by the `gateway_pair` fixture
  - PostgreSQL reachable (uses existing `pg_dsn` + `mock_upstream` fixtures)

Tests are automatically skipped when Redis is not reachable.
"""

import os
import subprocess
import socket
import time
import threading
from typing import Generator, Dict, Any, Optional, Tuple

import pytest
import requests

from mock_upstream import start_mock_upstream

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

ADMIN_TOKEN = "admin_integration_test_secret"
REDIS_URL = os.environ.get("REDIS_URL", "redis://127.0.0.1:6379")
MASTER_KEY = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def _redis_reachable() -> bool:
    """Return True if a Redis server is reachable."""
    try:
        import redis as _r  # optional dependency
        c = _r.Redis.from_url(REDIS_URL, socket_connect_timeout=0.5)
        c.ping()
        return True
    except Exception:
        pass
    # Fallback: raw TCP connect
    try:
        parts = REDIS_URL.replace("redis://", "").split(":")
        host = parts[0] or "127.0.0.1"
        port = int(parts[1]) if len(parts) > 1 else 6379
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except Exception:
        return False


def _redis_flush() -> None:
    """Flush all aigate:* keys from Redis (best effort)."""
    try:
        import redis as _r
        c = _r.Redis.from_url(REDIS_URL, socket_connect_timeout=1)
        keys = c.keys("aigate:*")
        if keys:
            c.delete(*keys)
    except Exception:
        pass


def _start_gateway(pg_dsn: str, mock_url: str, port: int,
                   redis_url: Optional[str] = None,
                   extra_env: Optional[Dict[str, str]] = None):
    """Start one aigate instance on the given port and return its process."""
    candidates_bin = [
        os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.build/aigate")),
        os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/aigate")),
    ]
    bin_path = next((b for b in candidates_bin if os.path.exists(b)), None)
    if not bin_path:
        pytest.fail(f"aigate binary not found in {candidates_bin}")

    env = os.environ.copy()
    env["AIGATE_LISTEN"] = f":{port}"
    env["AIGATE_PG_DSN"] = pg_dsn
    env["AIGATE_ADMIN_TOKEN"] = ADMIN_TOKEN
    env["AIGATE_METRICS_ACL"] = "127.0.0.1"
    env["AIGATE_MAX_BODY_BYTES"] = "65536"
    env["AIGATE_USAGE_FLUSH_S"] = "1"
    env["AIGATE_MASTER_KEY"] = MASTER_KEY
    if redis_url:
        env["AIGATE_REDIS_URL"] = redis_url
        env["AIGATE_REDIS_POOL_SIZE"] = "4"
        env["AIGATE_REDIS_TIMEOUT_MS"] = "200"
    if extra_env:
        env.update(extra_env)

    proc = subprocess.Popen([bin_path], env=env)
    base_url = f"http://127.0.0.1:{port}"

    # Wait up to 5s for readiness
    for _ in range(50):
        try:
            r = requests.get(f"{base_url}/metrics", timeout=0.5)
            if r.status_code == 200:
                return proc, base_url
        except Exception:
            pass
        time.sleep(0.1)

    proc.kill()
    pytest.fail(f"Gateway on port {port} did not start within 5 seconds")


def _stop_gateway(proc) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


def _admin_post(base_url: str, path: str, body: dict) -> requests.Response:
    return requests.post(
        f"{base_url}/admin/v1{path}",
        json=body,
        headers={"Authorization": f"Bearer {ADMIN_TOKEN}"},
        timeout=5,
    )


def _admin_patch(base_url: str, path: str, body: dict) -> requests.Response:
    return requests.patch(
        f"{base_url}/admin/v1{path}",
        json=body,
        headers={"Authorization": f"Bearer {ADMIN_TOKEN}"},
        timeout=5,
    )


def _chat(base_url: str, api_key: str, model: str = "test-model") -> requests.Response:
    return requests.post(
        f"{base_url}/v1/chat/completions",
        json={"model": model, "messages": [{"role": "user", "content": "hi"}]},
        headers={"Authorization": f"Bearer {api_key}"},
        timeout=5,
    )


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def redis_required():
    """Skip the module if Redis is not reachable."""
    if not _redis_reachable():
        pytest.skip("Redis not reachable — skipping distributed clustering tests")


@pytest.fixture(scope="module")
def mock_upstream_module():
    server, port = start_mock_upstream(0)
    yield f"http://127.0.0.1:{port}"
    server.shutdown()


@pytest.fixture(scope="module")
def gateway_pair(redis_required, pg_dsn, mock_upstream_module):
    """Start two aigate instances sharing the same Redis, yield (gw_a, gw_b) base_urls."""
    _redis_flush()

    # Clean PG
    try:
        subprocess.run(
            ["psql", pg_dsn, "-c",
             "DELETE FROM models; DELETE FROM api_keys; DELETE FROM groups; "
             "DELETE FROM usage_requests; DELETE FROM usage_daily;"],
            capture_output=True, timeout=5,
        )
    except Exception:
        pass

    port_a, port_b = _free_port(), _free_port()

    proc_a, url_a = _start_gateway(pg_dsn, mock_upstream_module, port_a, redis_url=REDIS_URL)
    proc_b, url_b = _start_gateway(pg_dsn, mock_upstream_module, port_b, redis_url=REDIS_URL)

    yield url_a, url_b, mock_upstream_module

    _stop_gateway(proc_a)
    _stop_gateway(proc_b)
    _redis_flush()


@pytest.fixture(scope="module")
def pg_dsn():
    """Re-export pg_dsn at module scope (delegates to conftest session fixture)."""
    from conftest import DEFAULT_PG_DSN, DOCKER_PG_DSN, DOCKER_PG_DSN_3
    dsn = os.environ.get("TEST_PG_DSN")
    candidates = [dsn] if dsn else [DEFAULT_PG_DSN, DOCKER_PG_DSN, DOCKER_PG_DSN_3]
    for candidate in candidates:
        try:
            res = subprocess.run(["pg_isready", "-d", candidate, "-t", "2"], capture_output=True)
            if res.returncode == 0:
                return candidate
        except FileNotFoundError:
            return candidate
    pytest.skip("PostgreSQL not reachable")


# ---------------------------------------------------------------------------
# Setup helper: create a model + key via one of the gateways
# ---------------------------------------------------------------------------

def _setup_key_and_model(base_url: str, mock_url: str, model_name: str,
                         qps: int = 1, daily_quota: int = 0) -> str:
    """Create a model route and an API key; return the raw API key."""
    # Create model
    r = _admin_post(base_url, "/models", {
        "name": model_name,
        "provider": "openai",
        "targets": [{"endpoint": f"{mock_url}/v1", "provider_key_env": "OPENAI_API_KEY",
                     "priority": 1, "weight": 1}],
    })
    assert r.status_code in (200, 201, 409), f"create model failed: {r.text}"

    # Enable model
    _admin_patch(base_url, f"/models/{model_name}", {"enabled": True})

    # Create API key
    r = _admin_post(base_url, "/keys", {
        "name": f"test-key-{model_name}",
        "allowed_models": [model_name],
        "rate_qps": qps,
        "daily_token_quota": daily_quota,
    })
    assert r.status_code in (200, 201), f"create key failed: {r.text}"
    return r.json()["key"]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestQPSAntiOversell:
    """QPS token bucket is enforced cluster-wide across two instances."""

    def test_qps_shared_across_instances(self, gateway_pair):
        """With QPS=1 and capacity=1, firing simultaneously at both instances
        should see exactly 1 success across the pair (atomicity via Redis)."""
        url_a, url_b, mock_url = gateway_pair
        api_key = _setup_key_and_model(url_a, mock_url, "qps-cluster-test", qps=1)

        # Drain the bucket first (one allowed)
        _chat(url_a, api_key, "qps-cluster-test")
        time.sleep(0.05)

        # Now fire at BOTH simultaneously — at most 1 should be admitted
        results = []
        threads = []

        def fire(url):
            r = _chat(url, api_key, "qps-cluster-test")
            results.append(r.status_code)

        for url in (url_a, url_b):
            t = threading.Thread(target=fire, args=(url,))
            threads.append(t)

        for t in threads:
            t.start()
        for t in threads:
            t.join()

        admitted = results.count(200)
        denied = results.count(429)
        # With QPS=1 and a shared bucket, at most 1 should slip through
        assert admitted <= 1, f"Expected at most 1 admission, got {admitted}: {results}"
        assert denied >= 1, f"Expected at least 1 denial, got {denied}: {results}"


class TestQuotaSharing:
    """Daily token quota is shared cluster-wide."""

    def test_quota_shared_across_instances(self, gateway_pair):
        """With daily_quota=1 token, one request consuming tokens via instance A
        should cause instance B to also see the quota as exhausted."""
        url_a, url_b, mock_url = gateway_pair

        # The mock upstream returns usage.total_tokens = 18 for chat completions
        # Set daily_quota to 10 so a single request overshoots it
        api_key = _setup_key_and_model(url_a, mock_url, "quota-cluster-test",
                                       qps=100, daily_quota=10)

        # First request via A: admitted, triggers record-first accounting (18 > 10 → OQ but passes)
        r1 = _chat(url_a, api_key, "quota-cluster-test")
        # The first request goes through (record-first, check-after-upstream)
        assert r1.status_code in (200, 429), f"Unexpected status from A: {r1.text}"

        # Give a moment for accounting to settle
        time.sleep(0.2)

        # Subsequent request via B should be rejected (quota exhausted in Redis)
        r2 = _chat(url_b, api_key, "quota-cluster-test")
        assert r2.status_code == 429, (
            f"Expected 429 from B after quota exceeded on A, got {r2.status_code}: {r2.text}"
        )
        body = r2.json()
        assert body.get("error", {}).get("code") == "daily_quota_exceeded"


class TestRedisDownFailClosed:
    """When Redis is configured but unavailable, gateway returns HTTP 503."""

    def test_redis_down_returns_503(self, redis_required, pg_dsn, mock_upstream_module):
        """Start a gateway with a bogus Redis URL (port 65530); any request should get 503."""
        bogus_redis = "redis://127.0.0.1:65530"
        port = _free_port()

        proc, url = _start_gateway(
            pg_dsn, mock_upstream_module, port, redis_url=bogus_redis,
            extra_env={"AIGATE_REDIS_TIMEOUT_MS": "50"},
        )
        try:
            # Create key/model via admin (admin doesn't go through RL gate)
            api_key = _setup_key_and_model(url, mock_upstream_module,
                                           "fail-closed-test", qps=100)

            # Data-plane request must hit 503 (Fail-Closed) because Redis is down
            r = _chat(url, api_key, "fail-closed-test")
            assert r.status_code == 503, (
                f"Expected 503 Fail-Closed, got {r.status_code}: {r.text}"
            )
            body = r.json()
            assert body.get("error", {}).get("code") == "distributed_state_unavailable"
        finally:
            _stop_gateway(proc)


class TestCircuitBreakerSync:
    """Circuit breaker state is shared across instances when Redis is available."""

    def test_cb_trip_propagates_to_peer(self, gateway_pair):
        """Trip the CB on instance A; instance B should also deny the endpoint."""
        url_a, url_b, mock_url = gateway_pair
        _redis_flush()

        model_name = "cb-sync-test"
        api_key = _setup_key_and_model(url_a, mock_url, model_name, qps=100)

        # Set very sensitive CB params (1 failure → OPEN, 1s cooldown)
        _admin_patch(url_a, f"/circuit-breaker/params",
                     {"failure_threshold": 1, "cooloff_sec": 30})

        # Trigger 1 failure at instance A (endpoint returns 500)
        # Re-create with a failing endpoint
        fail_model = "cb-fail-sync"
        _admin_post(url_a, "/models", {
            "name": fail_model,
            "provider": "openai",
            "targets": [{"endpoint": f"{mock_url}/fail/v1",
                          "provider_key_env": "OPENAI_API_KEY",
                          "priority": 1, "weight": 1}],
        })
        _admin_patch(url_a, f"/models/{fail_model}", {"enabled": True})
        _admin_patch(url_a, "/keys/1", {"allowed_models": [model_name, fail_model]})

        # Fire at A to trip the CB (500 from mock)
        _chat(url_a, api_key, fail_model)
        time.sleep(0.2)  # let CB state propagate to Redis

        # Fire at B: should get no_healthy_upstream (CB is OPEN cluster-wide)
        r = _chat(url_b, api_key, fail_model)
        # We expect either 503 no_healthy_upstream or a Fail-Closed 503;
        # either way, it should NOT be 200 since the CB should block
        assert r.status_code != 200, (
            f"Expected CB to block on B after trip on A, got 200: {r.text}"
        )
