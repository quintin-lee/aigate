# Reasoning Ecosystem v3 Implementation Plan

> **Date:** 2026-10-11  
> **Status:** Pending Review  
> **Target Version:** aigate v0.20.0  
> **Spec Reference:** [docs/superpowers/specs/2026-10-11-reasoning-ecosystem-v3-design.md](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-11-reasoning-ecosystem-v3-design.md)  
> **Strict Quality Gates:** Zero warnings with `-Wall -Wextra -Werror`, 100% pass on all unit & integration tests, Gitmoji commits.

---

## Phase Overview

| Phase | Component / Module | Key Files | Objectives |
|:---|:---|:---|:---|
| **Phase 1** | Database Schema Migration v20 & C Structs | `src/store/schema_sql.h`, `src/store/pg_store.h`, `src/store/pg_store.c` | Add `tag_emulation_default`, `daily_reasoning_quota` to models/keys/groups, update SQL migration & CRUD. |
| **Phase 2** | Gemini 2.0 Thinking Full-Duplex Bridge | `src/upstream/provider_gemini.c`, `src/upstream/provider_gemini.h` | Inbound `thinkingBudget` injection; outbound `parts[].thought` extraction in non-streaming & streaming state machine. |
| **Phase 3** | Model-Level Default Emulation & 3-Tier Resolution | `src/upstream/provider_adapter.h`, `src/upstream/provider_adapter.c` | Implement Header > Body > Route hierarchy, propagate `tag_emulation` setting across all providers. |
| **Phase 4** | Pricing Engine Extension & Usage Settlement | `src/core/aigate_core.c`, `src/core/aigate_core.h`, `src/server/admin_api.c` | Support `reasoning_mtok` in `pricing_json`, implement split cost formula with output/reasoning tokens. |
| **Phase 5** | Daily Reasoning Quota & Graceful Degradation | `src/policy/ratelimit.h`, `src/policy/ratelimit.c`, `src/core/pipeline_*.c` | Redis/Memory daily reasoning quota counter, graceful budget=0 override on quota exhaustion with headers. |
| **Phase 6** | Admin REST API & Observability | `src/server/admin_api.c`, `src/server/admin_api.h` | Model/Key/Group CRUD payload support, new `/reasoning_status` endpoint, telemetry fields. |
| **Phase 7** | Comprehensive Testing & Regression Verification | `tests/unit/test_unit.c`, `tests/` | Unit test cases for all new components, build verification with `-Werror`, full test suite execution. |

---

## Detailed Task Breakdown

### Phase 1: Database Schema Migration v20 & C Structs

#### Step 1.1: Schema SQL Migration v20
- **Files:** `src/store/schema_sql.h`
- **Actions:**
  - Bump `#define AIGATE_SCHEMA_VERSION 20`.
  - Add migration script:
    ```sql
    -- Migration v20: reasoning ecosystem v3: default tag emulation and daily reasoning quota
    ALTER TABLE models ADD COLUMN IF NOT EXISTS tag_emulation_default BOOLEAN NOT NULL DEFAULT FALSE;
    ALTER TABLE api_keys ADD COLUMN IF NOT EXISTS daily_reasoning_quota BIGINT NOT NULL DEFAULT 0;
    ALTER TABLE groups ADD COLUMN IF NOT EXISTS daily_reasoning_quota BIGINT NOT NULL DEFAULT 0;
    INSERT INTO schema_migrations(version) VALUES (20) ON CONFLICT (version) DO NOTHING;
    ```
- **Verification:** Compilation check via `ninja -C build`.

#### Step 1.2: C Structs & Mask Constants
- **Files:** `src/store/pg_store.h`
- **Actions:**
  - Add `bool tag_emulation_default;` to `model_rec_t`.
  - Add `long daily_reasoning_quota;` to `key_rec_t`.
  - Add `long daily_reasoning_quota;` to `group_rec_t`.
  - Add bitmask definitions if applicable (`MMASK_TAG_EMULATION_DEFAULT`, `KMASK_REASONING_QUOTA`, etc.).

#### Step 1.3: Store Layer Query & Update Bindings
- **Files:** `src/store/pg_store.c`
- **Actions:**
  - Update `SELECT` columns and parsers for `models`, `api_keys`, `groups` in `pg_store.c` (e.g. `COALESCE(tag_emulation_default, false)`).
  - Update `INSERT` and `UPDATE` statements to bind the new fields.
- **Verification:** `ctest --test-dir build -R unit` passing.

---

### Phase 2: Gemini 2.0 Thinking Full-Duplex Bridge

#### Step 2.1: Inbound `thinkingBudget` Configuration
- **Files:** `src/upstream/provider_gemini.c`
- **Actions:**
  - Enhance `generationConfig` construction in `provider_gemini_build`:
    - If `rcfg.enabled && rcfg.budget_tokens > 0`: inject `thinkingBudget = rcfg.budget_tokens`.
    - If `rcfg.enabled && rcfg.budget_tokens == 0`: inject `thinkingBudget = 0` (explicitly disable / graceful degradation fallback).

#### Step 2.2: Outbound Non-Streaming Translation
- **Files:** `src/upstream/provider_gemini.c`
- **Actions:**
  - In `provider_gemini_resp_to_openai`:
    - Loop through `parts[]`, checking `json_is_true(json_object_get(part, "thought"))`.
    - Separate into `accum_thought` and `accum_text`.
    - If `rcfg.tag_emulation` (or model default emulation):
      Format `content` as `<think>%s</think>%s`.
    - Else (raw format):
      Set `reasoning_content` to `accum_thought` and `content` to `accum_text`.
    - Track reasoning tokens in `usage.completion_tokens_details.reasoning_tokens`.

#### Step 2.3: Outbound Streaming State Machine
- **Files:** `src/upstream/provider_gemini.c`
- **Actions:**
  - Update `gemini_bridge_t`:
    - Add fields: `bool in_thought;`, `bool tag_opened;`, `bool tag_closed;`, `bool tag_emulation;`, `long reasoning_tokens;`.
  - In `gemini_bridge_process_line`:
    - Identify `thought: true` parts.
    - If `tag_emulation`:
      - First thought part: prepend `<think>` delta before text.
      - First non-thought part following thought: prepend `</think>\n` delta before text.
    - Else (raw mode):
      - Emit delta with `{"reasoning_content": ...}` when `thought: true`, and standard `{"content": ...}` otherwise.
    - On stream completion: if `tag_opened && !tag_closed`, emit `</think>\n` delta before `[DONE]`.

---

### Phase 3: Model-Level Default Emulation & 3-Tier Resolution

#### Step 3.1: Resolution Hierarchy Engine
- **Files:** `src/upstream/provider_adapter.h`, `src/upstream/provider_adapter.c`
- **Actions:**
  - Update `parse_reasoning_config` or add `parse_reasoning_config_ext`:
    - Signature: `int parse_reasoning_config_ext(json_t* req_body, const model_rec_t* route, const char* header_format, reasoning_config_t* out_cfg)`.
    - Priority 1: `header_format` (`"tag"` -> true, `"raw"` -> false).
    - Priority 2: `req_body` (`"thinking_format": "tag"|"raw"`, `"thinking_mode": "tag"|"raw"`).
    - Priority 3: `route->tag_emulation_default` (boolean).
  - Update all caller sites in `provider_gemini.c`, `provider_anthropic.c`, `provider_adapter.c`, etc.

---

### Phase 4: Pricing Engine Extension & Usage Settlement

#### Step 4.1: Separate Reasoning Pricing Calculation
- **Files:** `src/core/aigate_core.c`, `src/core/aigate_core.h`, `src/core/aigate_core_internal.h`
- **Actions:**
  - Extend `calc_req_cost` to `calc_req_cost_ext(const model_rec_t* route, long prompt, long completion, long cached, long reasoning)`:
    - Parse `reasoning_mtok` from `pricing_json` (fall back to `out_mtok` if missing or 0).
    - Deduct `reasoning` from `completion`: `normal_out = completion >= reasoning ? completion - reasoning : 0`.
    - Total cost = `(unhit_prompt * in_mtok + cached * in_mtok * discount + normal_out * out_mtok + reasoning * r_price) / 1e6`.
  - Keep `calc_req_cost(...)` wrapper delegating with `reasoning = 0` for backward compatibility.

#### Step 4.2: Admin Cost Attribution Update
- **Files:** `src/server/admin_api.c`
- **Actions:**
  - Update `cost_calc_tokens_to_cents` to account for `reasoning_mtok` when available.

---

### Phase 5: Daily Reasoning Quota & Graceful Degradation Engine

#### Step 5.1: Rate Limiter Daily Reasoning Quota Tracking
- **Files:** `src/policy/ratelimit.h`, `src/policy/ratelimit.c`
- **Actions:**
  - Extend `struct bucket` in `ratelimit.c` with `long daily_reasoning_used`.
  - Implement:
    - `long rl_remaining_daily_reasoning(ratelimit_t* rl, long key_id, long daily_reasoning_quota)`
    - `int rl_reserve_reasoning_tokens(ratelimit_t* rl, long key_id, long daily_reasoning_quota, long tokens)`
  - Support both local in-memory buckets and Redis keys (`"aigate:quota_rsn:%ld:%ld"`).

#### Step 5.2: Pipeline Graceful Degradation Logic
- **Files:** `src/core/pipeline_chat.c`, `src/core/pipeline_native.c`, `src/core/pipeline_responses.c`
- **Actions:**
  - Before upstream dispatch:
    - Evaluate effective quota: `krec.daily_reasoning_quota > 0 ? krec.daily_reasoning_quota : grec.daily_reasoning_quota`.
    - If effective quota > 0:
      - Call `rl_remaining_daily_reasoning(...)`.
      - If remaining <= 0:
        - Trigger degradation: override `rcfg.budget_tokens = 0`, `rcfg.enabled = false`.
        - Set request context flag: `req_ctx->reasoning_degraded = true`.
  - Response emission:
    - If `req_ctx->reasoning_degraded`:
      - Inject response headers:
        - `X-Aigate-Reasoning-Degraded: quota_exceeded`
        - `X-Aigate-Reasoning-Quota-Remaining: 0`
  - Usage settlement:
    - After completion (streaming or non-streaming), if `reasoning_tokens > 0`:
      `rl_reserve_reasoning_tokens(ac->rl, key_id, quota, reasoning_tokens)`.

---

### Phase 6: Admin REST API & Observability

#### Step 6.1: Admin API Model / Key / Group Handlers
- **Files:** `src/server/admin_api.c`
- **Actions:**
  - In `POST /api/v1/models` and `PUT /api/v1/models/{id}`:
    - Parse and write `tag_emulation_default` boolean.
    - Parse `pricing.reasoning_mtok` into `pricing_json`.
  - In `POST /api/v1/api_keys` and `PUT /api/v1/api_keys/{id}`:
    - Parse and write `daily_reasoning_quota`.
  - In `POST /api/v1/groups` and `PUT /api/v1/groups/{id}`:
    - Parse and write `daily_reasoning_quota`.
  - Add `GET /api/v1/api_keys/{id}/reasoning_status`:
    - Returns quota, used tokens today, remaining, and degraded status.

---

### Phase 7: Comprehensive Testing & Verification

#### Step 7.1: Unit Test Suite Expansion
- **Files:** `tests/unit/test_unit.c`
- **Actions:**
  - Test case `test_reasoning_resolution_3tier`: test priority of Header > Body > Route default.
  - Test case `test_pricing_reasoning_mtok`: verify cost calculation with separate reasoning rates and fallback behavior.
  - Test case `test_gemini_thinking_bridge`: test inbound `thinkingBudget` generation and outbound `parts[].thought` streaming tags.
  - Test case `test_daily_reasoning_quota_degradation`: verify quota decrement and zero-budget override.

#### Step 7.2: Build Hygiene & Regression Verification
- **Actions:**
  - Build with `-Wall -Wextra -Werror` zero warnings.
  - Run full `ctest --test-dir build` (all suites passed 100%).
  - Run integration pytest suite `pytest tests/integration/`.

---

## Git Commit Plan

1. `feat(store): 🗃️ add schema migration v20 for tag emulation default and daily reasoning quota`
2. `feat(gemini): 🧠 implement full-duplex gemini 2.0 thinking inbound and outbound stream bridge`
3. `feat(upstream): 🔀 implement 3-tier reasoning format resolution hierarchy`
4. `feat(meter): 💰 support independent reasoning token pricing in cost calculation`
5. `feat(policy): 🛡️ implement daily reasoning quota and graceful degradation`
6. `feat(admin): ⚙️ expose reasoning settings and status in admin rest api`
7. `test(reasoning): 🧪 add comprehensive unit tests for reasoning ecosystem v3`
