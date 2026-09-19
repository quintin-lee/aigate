#!/usr/bin/env bash
set -euo pipefail

# E2E Smoke test script for aigate (Task 11 Step 2)
# Requires a running PostgreSQL instance specified via TEST_PG_DSN.
# Example: TEST_PG_DSN="postgresql://postgres:postgres@localhost:5432/aigate_test" ./tests/integration/smoke.sh

TEST_PG_DSN="${TEST_PG_DSN:-postgresql://postgres:postgres@127.0.0.1:5432/aigate_test}"
PORT="${AIGATE_TEST_PORT:-18080}"
ADMIN_TOKEN="test_admin_secret_token_123"

echo "=== aigate E2E Smoke Test ==="
echo "Target DSN: $TEST_PG_DSN"
echo "Port:       $PORT"

# Check if PostgreSQL is reachable
if ! command -v pg_isready >/dev/null 2>&1; then
    echo "[WARN] pg_isready not found, continuing anyway..."
else
    if ! pg_isready -d "$TEST_PG_DSN" -t 2 >/dev/null 2>&1; then
        echo "[SKIP] PostgreSQL at $TEST_PG_DSN is not reachable. Skipping live smoke test."
        echo "To run live smoke test, start postgres: docker run --rm -p 5432:5432 -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=aigate_test postgres:16"
        exit 0
    fi
fi

BIN="./build/aigate"
if [ ! -f "$BIN" ]; then
    echo "[ERROR] $BIN not found. Please build first."
    exit 1
fi

export AIGATE_LISTEN="127.0.0.1:$PORT"
export AIGATE_PG_DSN="$TEST_PG_DSN"
export AIGATE_ADMIN_TOKEN="$ADMIN_TOKEN"
export AIGATE_METRICS_ACL="127.0.0.1"

echo "[1/5] Starting aigate in background..."
"$BIN" &
GATEWAY_PID=$!

cleanup() {
    echo "Stopping aigate (pid $GATEWAY_PID)..."
    kill -TERM "$GATEWAY_PID" 2>/dev/null || true
    wait "$GATEWAY_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for gateway to bind
sleep 1

echo "[2/5] Querying /metrics..."
METRICS=$(curl -s "http://127.0.0.1:$PORT/metrics")
echo "$METRICS" | grep -q "aigate_requests_total"
echo "  /metrics is functional."

echo "[3/5] Creating API key via Admin API..."
KEY_RESP=$(curl -s -X POST "http://127.0.0.1:$PORT/admin/v1/keys" \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"smoke-test-key","allowed_models":["mock-model"],"rate_qps":100}')

KEY_ID=$(echo "$KEY_RESP" | grep -o '"key_id":[0-9]*' | cut -d: -f2)
API_KEY=$(echo "$KEY_RESP" | grep -o '"plaintext":"[^"]*"' | cut -d'"' -f4)

echo "  Created key_id=$KEY_ID, plaintext=$API_KEY"

echo "[4/5] Registering mock model via Admin API..."
curl -s -X POST "http://127.0.0.1:$PORT/admin/v1/models" \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"mock-model","provider":"openai","endpoint":"http://127.0.0.1:19999/v1"}' >/dev/null

echo "  Model mock-model registered."

echo "[5/5] Querying /v1/models with client key..."
MODELS_RESP=$(curl -s "http://127.0.0.1:$PORT/v1/models" \
  -H "Authorization: Bearer $API_KEY")
echo "$MODELS_RESP" | grep -q "mock-model"
echo "  /v1/models returned mock-model successfully."

echo "=== Smoke test passed successfully! ==="
