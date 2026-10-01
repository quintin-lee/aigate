# Code Comments and Doxygen Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate all 135 Doxygen warnings (achieve 0 warnings), complete structural and architectural annotations across recent feature additions (`src/core`, `src/policy`, `src/upstream`), fix documentation markdown references, and verify zero functional logic impact.

**Architecture:** Pure documentation and comment enhancement without modifying any runtime control flow. Work is organized into 4 modular editing tasks followed by a unified 6-gate verification task.

**Tech Stack:** C11, Doxygen, Clang-Format, CMake, CTest.

**Spec:** [`docs/superpowers/specs/2026-10-01-code-comments-and-doxygen-alignment-design.md`](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-01-code-comments-and-doxygen-alignment-design.md)

---

### Task 1: Documentation Cross-References and Doxyfile Configuration

**Files:**
- Modify: `Doxyfile`
- Modify: `README.md`
- Modify: `docs/README.md`
- Modify: `docs/DEVELOPMENT.md`

- [ ] **Step 1: Update Doxyfile INPUT**
  Add `docs/ONBOARDING.md` and `docs/CONFIGURATION.md` to `INPUT` in `Doxyfile`:
  ```doxygen
  INPUT = src README.md docs/README.md docs/DEVELOPMENT.md docs/ONBOARDING.md docs/CONFIGURATION.md .env.example
  ```

- [ ] **Step 2: Fix Relative Markdown Links in Documentation**
  In `README.md`, `docs/README.md`, and `docs/DEVELOPMENT.md`, ensure references to `docs/ONBOARDING.md` and `docs/CONFIGURATION.md` use proper Markdown links (`[ONBOARDING.md](docs/ONBOARDING.md)` or relative `[ONBOARDING.md](ONBOARDING.md)` inside `docs/`) that Doxygen resolves cleanly without `unable to resolve reference` warnings.

- [ ] **Step 3: Verify Doc Warnings**
  Run: `doxygen Doxyfile 2>&1 | grep -E "(README|DEVELOPMENT|ONBOARDING|CONFIGURATION)"`
  Expected: No warnings output.

---

### Task 2: Core & Decoupled Pipeline Annotations (`src/core/`)

**Files:**
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `src/core/pipeline_embeddings.c`

- [ ] **Step 1: Document `chat_req_t` and Function Prototypes in `aigate_core_internal.h`**
  - Add module `@file` header describing pipeline decoupling context.
  - Add detailed Doxygen comments for each of the 17 fields in `chat_req_t` (`ac`, `rq`, `rc`, `krec`, `jbody`, `model`, `route`, `candidates`, `n_candidates`, `sanitized_body`, `sanitized_len`, `eff_body`, `eff_len`, `guardrail_act`, `cache_key`, `bypass_cache`, `no_store`).
  - Add `@brief`, `@param`, `@return` annotations to pipeline prototypes: `handle_embeddings`, `handle_chat_stream`, `handle_chat_sync`, `handle_models_list`, `prepare_chat_cache`, `record_usage_and_event`, `mono_ns`, `calc_req_cost`, `settle_success`, `failover_warn`, `gate_request`, `resolve_chat_target`, `chat_req_cleanup`, `stream_cache_acc_set_header`, `stream_cache_acc_write`, `cache_stream_replay`, `cache_store_stream`.

- [ ] **Step 2: Document `record_usage_and_event` in `aigate_core.c`**
  - Update Doxygen comment block to document all 12 parameters (`ac`, `key_id`, `model`, `status`, `prompt_tokens`, `completion_tokens`, `cached_tokens`, `reasoning_tokens`, `lat_ns`, `provider`, `guardrail_action`, `cost_usd`).

- [ ] **Step 3: Document `stream_feed_wrapper` and Helpers in `pipeline_chat.c`**
  - Document `struct stream_feed_wrapper` and all 7 members (`real_feed`, `real_bridge`, `lt`, `model`, `endpoint`, `t0`, `first_chunk_recorded`).
  - Document parameter `is_streaming` for `prepare_chat_cache`.
  - Document `handle_chat_stream`.

- [ ] **Step 4: Document `handle_embeddings` in `pipeline_embeddings.c`**
  - Add function docblock to `handle_embeddings` definition.

- [ ] **Step 5: Verify Core Warnings**
  Run: `doxygen Doxyfile 2>&1 | grep "src/core/"`
  Expected: No warnings output.

---

### Task 3: Policy & Middleware Subsystem Annotations (`src/policy/`)

**Files:**
- Modify: `src/policy/latency_tracker.h`
- Modify: `src/policy/latency_tracker.c`
- Modify: `src/policy/prompt_template.h`
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`

- [ ] **Step 1: Document `latency_tracker.h` & `latency_tracker.c`**
  - Document macros `LATENCY_TRACKER_WINDOW_SZ` and `LATENCY_TRACKER_MAX_ENTRIES`.
  - Document `typedef struct latency_tracker latency_tracker_t;`.
  - In `latency_tracker.c`, document `struct latency_entry` and all 10 members (`model`, `endpoint`, `samples_ms`, `head`, `count`, `cached_p95_ms`, `ewma_ms`, `total_requests`, `hedged_requests`, `rwlock`).
  - In `latency_tracker.c`, document `struct latency_tracker` and all 3 members (`entries`, `count`, `table_lock`).

- [ ] **Step 2: Document `prompt_template.h`**
  - Add `@file` and `@ingroup group_policy`.
  - Document `prompt_inject_mode_t` and enum members `PROMPT_MODE_PREPEND`, `PROMPT_MODE_APPEND`, `PROMPT_MODE_OVERRIDE`.
  - Document `prompt_template_t` and all 4 fields (`system_template`, `mode`, `prefix_user_prompt`, `suffix_user_prompt`).

- [ ] **Step 3: Document `guardrails.h` & `guardrails.c` Webhook Components**
  - In `guardrails.h`, document `guardrail_webhook_rule_t` members (`id`, `url`, `secret`, `timeout_ms`, `fail_mode`, `phase`).
  - In `guardrails.c`, document `struct webhook_resp_buf` members (`data`, `len`, `cap`).
  - Add `@brief` and parameter comments to internal webhook helper functions.

- [ ] **Step 4: Verify Policy Warnings**
  Run: `doxygen Doxyfile 2>&1 | grep "src/policy/"`
  Expected: No warnings output.

---

### Task 4: Upstream & Hedged Concurrency Annotations (`src/upstream/`)

**Files:**
- Modify: `src/upstream/upstream_hedged.h`
- Modify: `src/upstream/upstream_hedged.c`

- [ ] **Step 1: Document `upstream_hedged.h`**
  - Document macro `HEDGED_MAX_EXTRA_HEADERS`.
  - Document `hedged_endpoint_spec_t` and all 7 fields (`url`, `key`, `endpoint`, `payload`, `payload_len`, `extra_headers`, `n_extra_headers`).
  - Document `hedged_call_params_t` and all 10 fields (`model`, `primary`, `secondary`, `has_secondary`, `payload`, `payload_len`, `timeout_ms`, `delay_ms`, `budget_pct`, `lt`).
  - Document `hedged_call_result_t` and all 6 fields (`status`, `body`, `body_len`, `winning_target_idx`, `was_hedged`, `latency_ns`).

- [ ] **Step 2: Document `upstream_hedged.c`**
  - Document macro `RESP_MAX_LEN`.
  - Document `struct hedged_race_ctrl` and all 10 fields (`cond`, `ref_count`, `winning_idx`, `cancel_flags`, `done`, `http_status`, `rc`, `resp_body`, `resp_len`, `lat_ns`).
  - Document `typedef struct hedged_race_ctrl hedged_race_ctrl_t;`.
  - Document `struct worker_ctx` and all 9 fields (`ctrl`, `my_idx`, `url`, `key`, `payload`, `payload_len`, `extra_headers`, `n_extra_headers`, `timeout_ms`).
  - Document `typedef struct worker_ctx worker_ctx_t;`.

- [ ] **Step 3: Verify Upstream Warnings**
  Run: `doxygen Doxyfile 2>&1 | grep "src/upstream/"`
  Expected: No warnings output.

---

### Task 5: Comprehensive Verification, Formatting, and Git Commit

- [ ] **Step 1: Check Doxygen Zero-Warning Baseline**
  Run: `doxygen Doxyfile 2>&1 | grep "warning:"`
  Expected: 0 lines output (zero warnings across the entire repository).

- [ ] **Step 2: Check English Purity in C Source**
  Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/`
  Expected: 0 matches.

- [ ] **Step 3: Machine-Check Comment-Only Diff**
  Run: `git diff -U0 src/ | grep '^[+-]' | grep -vE '^[+-]{3}' | grep -vE '^[+-]\s*(\*|/\*|//|\*/|$)'`
  Expected: Empty output (confirms only comments and whitespace were modified).

- [ ] **Step 4: Build and Compiler Zero-Warning Check**
  Run: `cmake --build build -j`
  Expected: Exit code 0, zero warnings.

- [ ] **Step 5: Code Formatting Check**
  Run: `git diff --check`
  Expected: Clean output.

- [ ] **Step 6: Run Full Test Suite**
  Run: `ctest --test-dir build --output-on-failure`
  Expected: 6/6 tests passed (100%).

- [ ] **Step 7: Commit All Changes**
  Run: `git add Doxyfile README.md docs/ src/ && git commit -m "docs: 📝 complete Doxygen comments and achieve zero-warning baseline"`
