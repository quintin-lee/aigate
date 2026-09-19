# aigate Architecture Spec: Multi-Upstream Failover & Load Balancing

## 1. Overview & Goals

This specification defines the architecture for **Multi-Upstream Failover & Load Balancing** in `aigate`.
Currently, each model record maps to a single endpoint, provider, and upstream key. If that upstream provider experiences rate limiting (HTTP 429), server degradation (HTTP 5xx), or network outages, client requests immediately fail or exhaust retries against the same failing host.

### Goals
1. **Multi-Target Configuration**: A single logical model (e.g. `gpt-4o` or `deepseek-v3`) can bind up to 8 upstream targets with individual providers, endpoints, keys, weights, and priority tiers.
2. **Priority Tiering & Failover**: Automatic fallback across priority tiers (Tier 0 = Primary, Tier 1 = Backup 1, Tier 2 = Backup 2, etc.) on HTTP 429, HTTP 5xx, or network errors.
3. **Load Balancing within Priority**: Weighted round-robin or smooth round-robin among healthy targets within the same priority level.
4. **Circuit Breaker**: Automatic per-target circuit breaking: 3 consecutive failures open the circuit for a 30-second cool-off window, with half-open probe recovery.
5. **Full Observability & Admin Management**: Track failovers via Prometheus metrics (`aigate_failover_total`), expose circuit states in `/admin/v1/models`, and configure multi-upstream models in the Admin Console UI.
6. **Zero-Downtime Backward Compatibility**: Existing databases with single `(provider, endpoint, upstream_key_ref)` continue to function unchanged.

---

## 2. Data Model & Schema Migration v3

### 2.1 Database Schema (`schema/schema.sql`)
```sql
-- Migration v3: multi-upstream targets & load balancing policy
ALTER TABLE models ADD COLUMN IF NOT EXISTS targets JSONB NOT NULL DEFAULT '[]';
ALTER TABLE models ADD COLUMN IF NOT EXISTS lb_policy TEXT NOT NULL DEFAULT 'priority';
INSERT INTO schema_migrations(version) VALUES (3) ON CONFLICT (version) DO NOTHING;
```

### 2.2 In-Memory Target Representation (`src/pg_store.h`)
```c
#define MAX_TARGETS_PER_MODEL 8

typedef struct upstream_target {
    char provider[32];
    char endpoint[512];
    char upstream_key_ref[256];
    char upstream_key[1024]; /* resolved in-memory */
    int  weight;             /* default 1 */
    int  priority;           /* 0 = primary, 1 = backup 1, etc. */
} upstream_target_t;

typedef struct model_rec {
    long id;
    char name[128];
    char provider[32];              /* primary / fallback default */
    char endpoint[512];             /* primary / fallback default */
    char upstream_key_ref[256];     /* primary / fallback default */
    char default_params_json[1024];
    int  enabled;
    char upstream_key[1024];

    /* Multi-target additions */
    int               n_targets;
    upstream_target_t targets[MAX_TARGETS_PER_MODEL];
    char              lb_policy[16]; /* "priority", "round_robin", "weighted" */
} model_rec_t;
```

If `n_targets == 0`, `model_router` automatically populates `targets[0]` from `(provider, endpoint, upstream_key_ref, upstream_key, weight=1, priority=0)`.

---

## 3. Circuit Breaker Architecture (`src/circuit_breaker.h/.c`)

### 3.1 State Machine
Each target endpoint is tracked with three states:
- `CB_CLOSED` (Normal): All traffic is allowed. Consecutive failures counter increments on 429, 5xx, or network errors. When `consecutive_failures >= 3`, transitions to `CB_OPEN` and sets `open_until = now + 30s`.
- `CB_OPEN` (Tripped): Traffic is blocked. The router bypasses this target in favor of alternative targets. When `now >= open_until`, transitions to `CB_HALF_OPEN`.
- `CB_HALF_OPEN` (Trial): Allows a single probe request. If the request returns 200 OK, transitions to `CB_CLOSED` and resets failures to 0. If it fails (429/5xx), transitions back to `CB_OPEN` for another 30 seconds.

### 3.2 State Tracking Interface
```c
typedef enum {
    CB_CLOSED = 0,
    CB_OPEN = 1,
    CB_HALF_OPEN = 2,
} cb_state_t;

typedef struct circuit_breaker circuit_breaker_t;

circuit_breaker_t* cb_create(void);
void               cb_destroy(circuit_breaker_t* cb);

/* Check if endpoint is allowed to receive traffic */
cb_state_t cb_get_state(circuit_breaker_t* cb, const char* model, const char* endpoint);
bool       cb_allow_request(circuit_breaker_t* cb, const char* model, const char* endpoint);

/* Record execution outcome */
void cb_record_success(circuit_breaker_t* cb, const char* model, const char* endpoint);
void cb_record_failure(circuit_breaker_t* cb, const char* model, const char* endpoint, int http_status);
```

Thread safety is guaranteed using an internal mutex or lock striping across hash buckets.

---

## 4. Routing & Failover Pipeline (`src/aigate_core.c`)

### 4.1 Target Candidate Selection (`model_router_select_targets`)
Given a model with `N` targets:
1. Group targets by `priority` ascending (Priority 0, then 1, then 2...).
2. For each priority tier, filter out targets where `cb_allow_request(...) == false`.
3. Within the lowest priority tier having healthy targets:
   - If `lb_policy == "priority"` or only 1 target: use primary target, keep remaining healthy targets as sequential fallbacks.
   - If `lb_policy == "round_robin"`: select target via atomic index modulo count, keep others as fallbacks.
   - If `lb_policy == "weighted"`: select target via weighted random/counter, keep others as fallbacks.
4. If ALL targets across all tiers are tripped in the circuit breaker, fallback to the target with the earliest `open_until` expiry to prevent hard failure.

### 4.2 Non-Streaming Failover Execution
```c
for (int i = 0; i < n_candidates; i++) {
    upstream_target_t* target = &candidates[i];
    const provider_adapter_t* adapter = provider_find(target->provider);
    
    // 1. Build upstream request with candidate's target
    adapter->build_chat(...);
    
    // 2. Execute upstream call
    int status = 0;
    int urc = upstream_call_ext(target_url, target->upstream_key, ... &status ...);
    
    // 3. Evaluate outcome
    bool is_failover_code = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
    if (!is_failover_code && status < 400) {
        cb_record_success(ac->cb, model, target->endpoint);
        adapter->parse_chat_response(...);
        return aigate_write_json(...);
    }
    
    // 4. Record failure and trigger circuit breaker
    cb_record_failure(ac->cb, model, target->endpoint, status);
    
    // 5. If more candidates exist, failover to next candidate
    if (i + 1 < n_candidates) {
        metrics_inc_failover(model, target->provider, candidates[i+1].provider);
        AIGATE_LOG_WARN("failover for model %s from %s (%s) to %s (%s) due to status %d",
                        model, target->name, target->endpoint,
                        candidates[i+1].name, candidates[i+1].endpoint, status);
        continue;
    }
    
    // No more candidates: return upstream error
    return aigate_write_error(rc, status > 0 ? status : 502, ...);
}
```

### 4.3 Streaming Failover Execution
- Before headers are sent (`headers_sent == false`):
  - If upstream stream connection fails or returns HTTP 429/5xx, failover to the next candidate target occurs seamlessly!
- After headers are sent (`headers_sent == true`):
  - Client has already received the initial SSE response chunk.
  - Transparent switching is impossible; emit an SSE error event frame and `[DONE]` frame.

### 4.4 Embeddings Failover Execution
- `/v1/embeddings` shares the exact same candidate selection and failover loop across targets supporting `build_embeddings`.

---

## 5. Metrics & Observability

1. **Failover Counter**:
   `aigate_failover_total{model="<model>", from_provider="<prov1>", to_provider="<prov2>"}`
2. **Circuit Breaker State**:
   `aigate_circuit_breaker_tripped_total{model="<model>", endpoint="<endpoint>"}`
   `aigate_circuit_breaker_state{model="<model>", endpoint="<endpoint>", state="closed|open|half_open"}`

---

## 6. Admin Management & UI Console

1. **Admin REST API**:
   - `POST /admin/v1/models` and `PUT /admin/v1/models/{name}`: accept `targets: [...]` and `lb_policy: "priority"|"round_robin"|"weighted"`.
   - `GET /admin/v1/models`: includes `targets` array with real-time circuit state (`cb_state: "closed"|"open"|"half_open"`).
2. **Admin Web Console (`web/admin.html`)**:
   - Target list builder in Model modal (add/remove endpoints, set weights, set priorities).
   - Visual health badges: 🟢 Active, 🔴 Tripped (30s cooloff), 🟡 Probing.
