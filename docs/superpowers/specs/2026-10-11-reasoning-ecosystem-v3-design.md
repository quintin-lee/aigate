# Reasoning Ecosystem v3 Design Specification

> **Date:** 2026-10-11  
> **Status:** Approved / Ready for Implementation  
> **Target Version:** aigate v0.20.0  
> **Authors:** DeepMind / aigate Core Team  

---

## 1. Executive Summary & Motivation

In Reasoning Ecosystem v2, `aigate` established protocol-level reasoning passthrough across OpenAI `o1`/`o3`, Anthropic Claude 3.7 Sonnet Thinking, and DeepSeek-R1, standardizing on the `<think>` tag emulation and client-side format negotiation (`X-Thinking-Format` / `thinking_format: "tag" | "raw"`).

However, real-world deployment has surfaced three critical gaps:
1. **Gemini 2.0 Flash / Pro Thinking Support**: Gemini represents thinking output not as an independent top-level key or string, but as structured `parts[]` marked with `"thought": true` inside `candidates[0].content.parts[]`, and accepts inbound budget via `generationConfig.thinkingConfig.thinkingBudget`. Currently, these thinking tokens cannot be translated to and from OpenAI/Anthropic clients.
2. **Client-Agnostic Tag Emulation**: Many desktop/web LLM clients (e.g., standard Chat WebUI, legacy tools) do not send custom `X-Thinking-Format` headers or `thinking_format` parameters, requiring server-side / model-level configuration to default to `<think>` tag emulation for transparent parsing.
3. **Reasoning Quota & Commercial Pricing Deficits**: Upstream providers charge substantially higher rates for reasoning tokens. Furthermore, unbounded reasoning requests can exhaust daily budgets within minutes. Without dedicated token pricing, daily quotas, and graceful degradation (fallback to non-thinking instead of hard 429 errors), organizations face budget blowouts or broken user workflows.

Reasoning Ecosystem v3 directly addresses these needs with a full-layer decoupled, adaptive reasoning system.

---

## 2. Architecture Overview & System Topology

```
                  ┌──────────────────────────────────────────────┐
                  │          Downstream Client Requests          │
                  │ (OpenAI / Anthropic / Chat WebUI / Cursor)   │
                  └──────────────────────┬───────────────────────┘
                                         │
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │ 1. Inbound Parsing & 3-Tier Format Decision   │
                  │    - Header: X-Thinking-Format               │
                  │    - Body: thinking_format                   │
                  │    - Model Route Default: tag_emulation_def │
                  └──────────────────────┬───────────────────────┘
                                         │
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │ 2. Daily Reasoning Quota & Graceful Degrade  │
                  │    - Check daily_reasoning_quota (Key/Group) │
                  │    - If exceeded: override budget=0          │
                  │      Inject degraded header                  │
                  └──────────────────────┬───────────────────────┘
                                         │
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │ 3. Protocol Translation & Adapter Bridge     │
                  │    - OpenAI o1/o3 (reasoning_effort)         │
                  │    - Anthropic Claude 3.7 (thinking.budget)  │
                  │    - Gemini 2.0 (thinkingConfig.thinkingBud) │
                  └──────────────────────┬───────────────────────┘
                                         │
                     ┌───────────────────┼───────────────────┐
                     ▼                   ▼                   ▼
               OpenAI Upstream     Anthropic Upstream   Gemini Upstream
                     │                   │                   │
                     └───────────────────┼───────────────────┘
                                         │
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │ 4. Outbound Streaming & Tag Emulation Engine │
                  │    - Gemini parts[].thought state machine    │
                  │    - Stream/buffered <think> tag synthesis   │
                  │    - Usage separation & token accounting     │
                  └──────────────────────┬───────────────────────┘
                                         │
                                         ▼
                  ┌──────────────────────────────────────────────┐
                  │ 5. Usage Metering & Billing Settlement       │
                  │    - pricing_json reasoning_mtok calculation │
                  │    - Post-call atomic quota deduction        │
                  │    - Audit logging (reasoning_tokens)        │
                  └──────────────────────────────────────────────┘
```

---

## 3. Database Schema Migration v20 (`schema_sql.h`)

To support model-level emulation defaults and daily reasoning quotas, the schema version advances from **19 to 20**.

### 3.1 Migration DDL
```sql
-- Migration from schema version 19 to 20
ALTER TABLE models ADD COLUMN tag_emulation_default BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE api_keys ADD COLUMN daily_reasoning_quota BIGINT NOT NULL DEFAULT 0;
ALTER TABLE groups ADD COLUMN daily_reasoning_quota BIGINT NOT NULL DEFAULT 0;

-- Track schema version
UPDATE schema_version SET version = 20 WHERE id = 1;
```

### 3.2 C Structs and Bitmask Updates (`store.h`)
```c
/* Model attribute update bitmask */
#define MMASK_TAG_EMULATION_DEFAULT  (1ULL << 15)

/* API Key attribute update bitmask */
#define KMASK_REASONING_QUOTA        (1ULL << 8)

/* Group attribute update bitmask */
#define GMASK_REASONING_QUOTA        (1ULL << 3)

typedef struct model_entry_s {
    char id[64];
    char name[128];
    char target_model[128];
    /* ... existing fields ... */
    bool tag_emulation_default;      /* Default <think> emulation when format not specified */
    char pricing_json[1024];         /* Extended pricing JSON schema */
} model_entry_t;

typedef struct api_key_entry_s {
    char id[64];
    char key_hash[65];
    /* ... existing fields ... */
    int64_t daily_reasoning_quota;   /* Daily reasoning token limit (0 = unlimited) */
} api_key_entry_t;

typedef struct group_entry_s {
    char id[64];
    char name[128];
    /* ... existing fields ... */
    int64_t daily_reasoning_quota;   /* Inherited daily reasoning token limit */
} group_entry_t;
```

---

## 4. Gemini 2.0 Flash / Pro Thinking Bidirectional Bridge

### 4.1 Inbound Translation (`provider_gemini.c`)
Gemini 2.0 Thinking models configure reasoning via `generationConfig.thinkingConfig.thinkingBudget`:
* `thinkingBudget > 0`: Enables dynamic reasoning up to budget tokens.
* `thinkingBudget = 0`: Disables thinking completely (standard generation).
* `thinkingBudget = -1`: Dynamic / model-managed thinking budget.

```c
/* Inbound Gemini Request Translation */
cJSON *gen_cfg = cJSON_GetObjectItem(root, "generationConfig");
if (!gen_cfg) {
    gen_cfg = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "generationConfig", gen_cfg);
}

if (rcfg->enabled && rcfg->budget_tokens > 0) {
    cJSON *thinking_cfg = cJSON_CreateObject();
    cJSON_AddNumberToObject(thinking_cfg, "thinkingBudget", (double)rcfg->budget_tokens);
    cJSON_AddItemToObject(gen_cfg, "thinkingConfig", thinking_cfg);
} else if (rcfg->enabled && rcfg->budget_tokens == 0) {
    /* Explicitly disable thinking or handle degraded mode */
    cJSON *thinking_cfg = cJSON_CreateObject();
    cJSON_AddNumberToObject(thinking_cfg, "thinkingBudget", 0);
    cJSON_AddItemToObject(gen_cfg, "thinkingConfig", thinking_cfg);
}
```

### 4.2 Outbound Streaming State Machine
In Gemini 2.0 streaming, chunks arrive as:
```json
{
  "candidates": [
    {
      "content": {
        "parts": [
          { "text": "Analyzing the constraints...", "thought": true }
        ],
        "role": "model"
      }
    }
  ]
}
```
When thinking finishes and standard generation begins:
```json
{
  "candidates": [
    {
      "content": {
        "parts": [
          { "text": "Here is the solution to your problem:" }
        ]
      }
    }
  ]
}
```

#### Adapter Streaming State (`gemini_stream_state_t`)
```c
typedef struct {
    bool in_thought;
    bool tag_opened;
    bool tag_closed;
    reasoning_format_t format;
    size_t accumulated_thought_tokens;
} gemini_reasoning_stream_t;
```

**State Transition Rules**:
1. When a chunk has `parts[i].thought == true` and `format == REASONING_FORMAT_TAG`:
   - If `!tag_opened`: Emit `<think>` prefix into the streaming content chunk, set `tag_opened = true`, `in_thought = true`.
   - Emit text content inside the tag.
2. When a chunk has `parts[i].thought == false` (or `thought` property absent) while `in_thought == true`:
   - If `tag_opened && !tag_closed`: Emit `</think>\n` closing tag into the streaming content chunk, set `in_thought = false`, `tag_closed = true`.
   - Emit normal text content.
3. On stream end (`data: [DONE]`):
   - If `tag_opened && !tag_closed`: Safety close with `</think>\n`.

---

## 5. Model-Level Default `<think>` Emulation & 3-Tier Resolution

### 5.1 Three-Tier Resolution Order
When processing incoming client requests, the output format for reasoning content is resolved using a strict 3-tier hierarchy:

1. **Tier 1 (Explicit Request Header)**:
   - `X-Thinking-Format: tag` -> Force tag emulation (`REASONING_FORMAT_TAG`).
   - `X-Thinking-Format: raw` -> Force separate raw field (`REASONING_FORMAT_RAW`).
2. **Tier 2 (Request Body Parameter)**:
   - `body.thinking_format == "tag"` -> `REASONING_FORMAT_TAG`.
   - `body.thinking_format == "raw"` -> `REASONING_FORMAT_RAW`.
3. **Tier 3 (Model / Route Default)**:
   - `model->tag_emulation_default == true` -> `REASONING_FORMAT_TAG`.
   - Otherwise -> `REASONING_FORMAT_RAW`.

```c
reasoning_format_t resolve_reasoning_format(
    const http_request_t *req,
    const cJSON *body_json,
    const model_entry_t *model
) {
    /* 1. Header priority */
    const char *h_fmt = http_request_get_header(req, "X-Thinking-Format");
    if (h_fmt) {
        if (strcasecmp(h_fmt, "tag") == 0) return REASONING_FORMAT_TAG;
        if (strcasecmp(h_fmt, "raw") == 0) return REASONING_FORMAT_RAW;
    }

    /* 2. Body priority */
    if (body_json) {
        cJSON *b_fmt = cJSON_GetObjectItem(body_json, "thinking_format");
        if (cJSON_IsString(b_fmt) && b_fmt->valuestring) {
            if (strcasecmp(b_fmt->valuestring, "tag") == 0) return REASONING_FORMAT_TAG;
            if (strcasecmp(b_fmt->valuestring, "raw") == 0) return REASONING_FORMAT_RAW;
        }
    }

    /* 3. Model default */
    if (model && model->tag_emulation_default) {
        return REASONING_FORMAT_TAG;
    }

    return REASONING_FORMAT_RAW;
}
```

---

## 6. Separate Pricing, Daily Quota & Graceful Degradation Engine

### 6.1 Pricing Engine Extension (`pricing_json`)
In `models.pricing_json`, a dedicated `reasoning_mtok` field represents the cost per 1M reasoning tokens:
```json
{
  "input_mtok": 0.50,
  "output_mtok": 2.00,
  "reasoning_mtok": 8.00,
  "cached_input_mtok": 0.25
}
```

#### Cost Calculation Formula (`usage_meter.c`)
Standard completions bundle reasoning tokens within total `completion_tokens`. Therefore:
$$\text{normal\_output\_tokens} = \max(0, \text{completion\_tokens} - \text{reasoning\_tokens})$$
$$\text{Cost} = \frac{\text{prompt\_tokens} \times P_{\text{input}} + \text{normal\_output\_tokens} \times P_{\text{output}} + \text{reasoning\_tokens} \times P_{\text{reasoning}}}{1,000,000}$$

If `reasoning_mtok` is missing or $0$, $P_{\text{reasoning}} = P_{\text{output}}$.

---

### 6.2 Daily Quota & Graceful Degradation

#### Quota Hierarchy
- `api_keys.daily_reasoning_quota > 0`: Direct Key Quota.
- If Key quota is $0$, inherit `groups.daily_reasoning_quota > 0`.
- If both are $0$, unlimited.

#### Degradation Flow
When `used_reasoning_today >= daily_reasoning_quota`:
1. **Never return HTTP 429**: Avoid breaking ongoing developer workflows or terminal agents.
2. **Override Reasoning Budget**:
   - `rcfg.budget_tokens = 0`
   - `rcfg.enabled = false`
3. **Upstream Protocol Adjustments**:
   - OpenAI: Remove `reasoning_effort`.
   - Anthropic: Omit `thinking` struct.
   - Gemini: Set `thinkingBudget = 0`.
4. **Response Headers Injected**:
   - `X-Aigate-Reasoning-Degraded: quota_exceeded`
   - `X-Aigate-Reasoning-Quota-Daily: <limit>`
   - `X-Aigate-Reasoning-Quota-Used: <current>`
   - `X-Aigate-Reasoning-Quota-Remaining: 0`
5. **Telemetry & Audit**: Mark `req_audit_logs.reasoning_degraded = 1`.

---

## 7. Web Console UI & Admin API Specifications

### 7.1 Admin REST API
- `POST /api/v1/models` & `PUT /api/v1/models/{id}`:
  - Accepts `tag_emulation_default: bool` and `pricing.reasoning_mtok: number`.
- `POST /api/v1/api_keys` & `PUT /api/v1/api_keys/{id}`:
  - Accepts `daily_reasoning_quota: int64`.
- `POST /api/v1/groups` & `PUT /api/v1/groups/{id}`:
  - Accepts `daily_reasoning_quota: int64`.
- `GET /api/v1/api_keys/{id}/reasoning_status`:
  - Returns current quota limit, used tokens today, reset timestamp, and degradation status.

### 7.2 Web Console UI
- **Model Modal**:
  - Checkbox: `[x] Default Tag Emulation (<think>)`.
  - Input: `Reasoning Price ($/M tokens)`.
- **API Key & Group Modals**:
  - Input: `Daily Reasoning Quota (tokens)`.
  - Quick select buttons: `[Unlimited]` `[100K]` `[500K]` `[2M]` `[10M]`.
- **Dashboard Metrics**:
  - Reasoning Token Daily Consumption Graph.
  - Percentage of Reasoning Tokens vs. Total Output Tokens.
  - Degradation Event Count Tracker.

---

## 8. Verification & Test Plan

1. **Unit Tests (`tests/test_reasoning_v3.c`)**:
   - Test 3-tier format resolution under all combinations of Header, Body, and Model Default.
   - Test Gemini outbound streaming state machine for split thoughts, contiguous thoughts, and edge-case closures.
   - Test usage cost computation with and without `reasoning_mtok`.
2. **Integration Tests (`tests/integration/test_reasoning_quota.py`)**:
   - Mock upstream Gemini and Anthropic servers.
   - Verify graceful degradation overrides budget to 0 when daily limit is reached.
   - Verify `X-Aigate-Reasoning-Degraded` headers present on responses.
3. **Admin API & Migration Tests**:
   - Schema upgrade v19 -> v20 verification on SQLite / PostgreSQL.
   - Roundtrip CRUD tests for `tag_emulation_default` and `daily_reasoning_quota`.
