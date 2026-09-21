"""Pytest fixtures for aigate end-to-end integration tests."""

import os
import subprocess
import time
import pytest
import requests
from typing import Generator, Dict, Any

from mock_upstream import start_mock_upstream

DEFAULT_PG_DSN = "postgresql://postgres:postgres@127.0.0.1:5432/aigate_test"
DOCKER_PG_DSN = "postgresql://aigate:changeme@172.39.4.2:5432/aigate"
ADMIN_TOKEN = "admin_integration_test_secret"

@pytest.fixture(scope="session", autouse=True)
def ensure_no_proxy():
    os.environ["no_proxy"] = "127.0.0.1,localhost," + os.environ.get("no_proxy", "")

@pytest.fixture(scope="session")
def pg_dsn() -> str:
    dsn = os.environ.get("TEST_PG_DSN")
    candidates = [dsn] if dsn else [DEFAULT_PG_DSN, DOCKER_PG_DSN]
    for candidate in candidates:
        try:
            res = subprocess.run(["pg_isready", "-d", candidate, "-t", "2"], capture_output=True)
            if res.returncode == 0:
                return candidate
        except FileNotFoundError:
            return candidate
    pytest.skip(f"PostgreSQL not reachable")

@pytest.fixture(scope="session")
def mock_upstream():
    server, port = start_mock_upstream(0)
    yield f"http://127.0.0.1:{port}"
    server.shutdown()

@pytest.fixture(scope="session")
def gateway(pg_dsn: str, mock_upstream: str) -> Generator[Dict[str, Any], None, None]:
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        port = s.getsockname()[1]

    bin_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/aigate"))
    if not os.path.exists(bin_path):
        pytest.fail(f"aigate binary not found at {bin_path}. Run cmake --build build first.")

    # Clean DB state for tests
    try:
        subprocess.run(["psql", pg_dsn, "-c", "DELETE FROM models; DELETE FROM api_keys; DELETE FROM usage_daily;"], capture_output=True)
    except Exception:
        pass

    env = os.environ.copy()
    env["AIGATE_LISTEN"] = f":{port}"
    env["AIGATE_PG_DSN"] = pg_dsn
    env["AIGATE_ADMIN_TOKEN"] = ADMIN_TOKEN
    env["AIGATE_METRICS_ACL"] = "127.0.0.1"
    env["AIGATE_MAX_BODY_BYTES"] = "2048"
    env["AIGATE_MASTER_KEY"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    env["ANTHROPIC_API_KEY"] = "sk-ant-test-key"

    proc = subprocess.Popen([bin_path], env=env)
    base_url = f"http://127.0.0.1:{port}"

    # Wait for gateway to respond
    ready = False
    for _ in range(30):
        try:
            r = requests.get(f"{base_url}/metrics", timeout=1)
            if r.status_code == 200:
                ready = True
                break
        except Exception:
            time.sleep(0.1)

    if not ready:
        proc.kill()
        pytest.fail("Gateway did not start within 3 seconds")

    yield {
        "base_url": base_url,
        "admin_token": ADMIN_TOKEN,
        "mock_upstream": mock_upstream,
    }

    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
