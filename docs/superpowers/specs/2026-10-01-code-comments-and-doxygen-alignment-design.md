# Code Comments and Doxygen Alignment Design

**Date:** 2026-10-01  
**Status:** Approved  
**Topic:** Code Comments and Documentation Alignment (`docs` & `src` Doxygen zero-warning baseline)

---

## 1. Motivation and Goals

Recent major feature additions (tool calling protocol translation, multimodal vision, core pipeline decoupling into `pipeline_*.c`, middleware filter chain, prompt templates, webhook-based guardrail moderation, adaptive latency-aware routing, and hedged speculative execution) introduced new structs, macros, and internal helper routines without complete Doxygen annotations.

Additionally, relative cross-references between top-level documentation files (`README.md`, `docs/README.md`, `docs/DEVELOPMENT.md`) and unindexed documents (`docs/ONBOARDING.md`, `docs/CONFIGURATION.md`) currently trigger unresolved reference warnings during Doxygen generation.

### Key Goals
1. **Doxygen Zero Warnings**: Eliminate all 135 existing Doxygen warnings (`doxygen Doxyfile 2>&1 | grep "warning:"` outputs 0 lines).
2. **Unified English Comment Standard**: Maintain the repo's established standard of 100% English Doxygen docstrings across `src/` (zero CJK characters in source code comments).
3. **Comprehensive Structural Documentation**: Provide complete `@brief` and field-level `/**< ... */` annotations for all newly added structs, enums, unions, and macros.
4. **Behavioral and Concurrency Contracts**: Explicitly document ownership, lifecycle, and thread-safety models for key components (such as `chat_req_t`, `latency_tracker_t`, and `upstream_hedged_t`).
5. **Zero Logic Invasiveness**: Strictly restrict all C code modifications to comments and whitespace, guaranteeing zero runtime or binary functional side effects.

---

## 2. Scope Matrix and File List

### 2.1 Configuration and Documentation Files
- **`Doxyfile`**:
  - Add `docs/ONBOARDING.md` and `docs/CONFIGURATION.md` to `INPUT`.
- **`README.md`**:
  - Fix relative links pointing to `docs/CONFIGURATION.md` and `docs/ONBOARDING.md` so Doxygen's markdown parser resolves them as valid page/section references.
- **`docs/README.md`**:
  - Align internal markdown links to `ONBOARDING.md` and `CONFIGURATION.md`.
- **`docs/DEVELOPMENT.md`**:
  - Fix markdown reference to `ONBOARDING.md`.

### 2.2 Core & Decoupled Pipelines (`src/core/`)
- **`src/core/aigate_core_internal.h`**:
  - Add module-level `@file` header describing pipeline decoupling architecture.
  - Document all 17 members of `chat_req_t` with explicit ownership and role notes.
  - Document all declared pipeline handlers and utilities (`handle_embeddings`, `handle_chat_stream`, `handle_chat_sync`, `handle_models_list`, `prepare_chat_cache`, `record_usage_and_event`, `mono_ns`, `calc_req_cost`, `settle_success`, `failover_warn`, etc.) with full `@param` and `@return` tags.
- **`src/core/aigate_core.c`**:
  - Document all parameters of `record_usage_and_event`.
- **`src/core/pipeline_chat.c`**:
  - Document `struct stream_feed_wrapper` and all 7 members (`real_feed`, `real_bridge`, `lt`, `model`, `endpoint`, `t0`, `first_chunk_recorded`).
  - Document parameter `is_streaming` for `prepare_chat_cache`.
  - Document `handle_chat_stream`.
- **`src/core/pipeline_embeddings.c`**:
  - Document `handle_embeddings` definition.

### 2.3 Policy and Middleware (`src/policy/`)
- **`src/policy/latency_tracker.h` / `src/policy/latency_tracker.c`**:
  - Document macros `LATENCY_TRACKER_WINDOW_SZ` and `LATENCY_TRACKER_MAX_ENTRIES`.
  - Document `latency_entry_t` / `struct latency_entry` and all 10 members (`model`, `endpoint`, `samples_ms`, `head`, `count`, `cached_p95_ms`, `ewma_ms`, `total_requests`, `hedged_requests`, `rwlock`).
  - Document `struct latency_tracker` and all 3 members (`entries`, `count`, `table_lock`).
  - Note threading safety: fine-grained per-entry `rwlock` for sampling reads vs. metric writes; table-level `table_lock` for dynamic entry allocation.
- **`src/policy/prompt_template.h`**:
  - Add `@file` and `@ingroup group_policy`.
  - Document `prompt_inject_mode_t` and enum values (`PROMPT_MODE_PREPEND`, `PROMPT_MODE_APPEND`, `PROMPT_MODE_OVERRIDE`).
  - Document `prompt_template_t` and its 4 fields (`system_template`, `mode`, `prefix_user_prompt`, `suffix_user_prompt`).
- **`src/policy/guardrails.h` / `src/policy/guardrails.c`**:
  - Document `guardrail_webhook_rule_t` members (`id`, `url`, `secret`, `timeout_ms`, `fail_mode`, `phase`).
  - Document `struct webhook_resp_buf` members (`data`, `len`, `cap`) in `guardrails.c`.
  - Add `@brief` and parameter comments to internal webhook invocation helpers.

### 2.4 Upstream and Hedged Requests (`src/upstream/`)
- **`src/upstream/upstream_hedged.h` / `src/upstream/upstream_hedged.c`**:
  - Document macro `HEDGED_MAX_EXTRA_HEADERS`.
  - Document `hedged_endpoint_spec_t` and all 7 fields (`url`, `key`, `endpoint`, `payload`, `payload_len`, `extra_headers`, `n_extra_headers`).
  - Document `hedged_call_params_t` and all 9 fields (`model`, `primary`, `secondary`, `has_secondary`, `payload`, `payload_len`, `timeout_ms`, `delay_ms`, `budget_pct`, `lt`).
  - Document `hedged_call_result_t` and all 6 fields (`status`, `body`, `body_len`, `winning_target_idx`, `was_hedged`, `latency_ns`).
  - Document macro `RESP_MAX_LEN`.
  - Document `struct hedged_race_ctrl` and all 10 fields (`cond`, `ref_count`, `winning_idx`, `cancel_flags`, `done`, `http_status`, `rc`, `resp_body`, `resp_len`, `lat_ns`).
  - Document `struct worker_ctx` and all 9 fields (`ctrl`, `my_idx`, `url`, `key`, `payload`, `payload_len`, `extra_headers`, `n_extra_headers`, `timeout_ms`).

---

## 3. Comment Formatting Standards

All comments must adhere to standard Javadoc/Doxygen syntax:
1. **File Headers**:
   ```c
   /**
    * @file filename.h
    * @ingroup group_name
    * @brief One-line summary of file responsibility.
    *
    * Detailed architectural role and component relationships.
    */
   ```
2. **Struct Definitions and Members**:
   ```c
   /**
    * @brief Short description of the structure.
    */
   typedef struct sample_struct {
       int    id;     /**< Unique identifier. */
       char*  name;   /**< Human-readable name (caller-owned). */
   } sample_struct_t;
   ```
3. **Function Contracts**:
   ```c
   /**
    * @brief Perform an operation.
    *
    * @param[in]     ctx     Pointer to initialized context.
    * @param[out]    out_buf Buffer to store output.
    * @return 0 on success, negative error code on failure.
    * @note Thread safety: Safe to call concurrently across multiple threads.
    */
   ```
4. **Pure English**: No non-ASCII CJK characters in `src/`.

---

## 4. Verification and Validation Gates

To ensure total code hygiene and prevent regression:
1. **Doxygen Gate**:
   `doxygen Doxyfile 2>&1 | grep "warning:"` must produce **0** output lines.
2. **Diff Verification**:
   `git diff` across `src/` must only modify comment blocks and non-functional whitespace.
3. **English Check**:
   `grep -rP '[\x{4e00}-\x{9fff}]' src/` must return nothing.
4. **Compilation Gate**:
   `cmake --build build -j` must build cleanly with `-Wall -Wextra -Werror` and zero compiler warnings.
5. **Code Style Gate**:
   `git diff --check` and `cmake --build build --target format` must pass without introducing style violations.
6. **Test Suite Gate**:
   `ctest --test-dir build --output-on-failure` must pass 6/6 test suites (100% pass).
