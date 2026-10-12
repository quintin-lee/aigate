# Reasoning Ecosystem v4 Implementation Plan

> **Date:** 2026-10-12  
> **Status:** Ready for Implementation  
> **Target Version:** aigate v0.21.0  
> **Spec Reference:** [docs/superpowers/specs/2026-10-12-reasoning-ecosystem-v4-design.md](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-12-reasoning-ecosystem-v4-design.md)  
> **Strict Quality Gates:** Zero warnings with `-Wall -Wextra -Werror`, 100% pass on all unit & integration tests (`ctest`), Gitmoji commits.

---

## Phase Overview

| Phase | Component / Module | Key Files | Objectives |
|:---|:---|:---|:---|
| **Phase 1** | Claude 3.7 Extended Thinking & SSE Adapter | `src/upstream/provider_anthropic.c`, `src/upstream/provider_adapter.c`, `tests/unit/upstream/test_provider_reasoning.c` | Expand thinking budget to 128k, auto-lift `max_tokens`, handle `signature_delta` and `redacted_thinking`, graceful degradation suppression. |
| **Phase 2** | OpenAI o1 / o3-mini Protocol Bridging & Stream Normalization | `src/upstream/provider_openai.c`, `src/upstream/provider_adapter.c`, `tests/unit/upstream/test_provider_reasoning.c` | Support `reasoning_effort` and `max_completion_tokens`, bridge `delta.reasoning_content` and fallback `delta.thought` to 3-tier tag emulation. |
| **Phase 3** | Web Console Models, Keys & Groups Quota Governance | `web/admin.html`, `tests/unit/server/test_admin_ui.c` | Add `tag_emulation_default` and `reasoning_mtok` to Models; add `daily_reasoning_quota` and degradation badges to Keys & Groups. |
| **Phase 4** | Playground Deep Reasoning Workbench & Overview Cards | `web/admin.html`, `tests/unit/server/test_admin_ui.c` | Add Reasoning Format selector, 128k slider, collapsible thinking accordion, amber degraded alert banner, and telemetry cards. |
| **Phase 5** | Regression Build & Full Test Suite Verification | Entire codebase | Run clean compilation under `-Wall -Wextra -Werror` and verify 100% pass across all CTest suites. |

---

## Detailed Task Breakdown

### Phase 1: Claude 3.7 Extended Thinking & SSE Adapter

#### Step 1.1: Budget Ceiling Expansion & Request Mutation
- **Files:** `src/upstream/provider_adapter.c`, `src/upstream/provider_anthropic.c`
- **Actions:**
  - In `src/upstream/provider_adapter.c`: Ensure `parse_reasoning_config_ext` parses `thinking.budget_tokens` up to 128,000 tokens (clamp > 128000 to 128000; ensure positive minimum 1024). Support explicit `"tag_emulation": true/false` directly in request body.
  - In `src/upstream/provider_anthropic.c` (`anthropic_to_upstream_body`):
    - Update `max_tokens` elevation logic: when `rcfg.enabled && rcfg.budget_tokens > 0`, ensure `max_tokens` is strictly greater than `rcfg.budget_tokens`. If `max_tokens <= rcfg.budget_tokens`, set `max_tokens = rcfg.budget_tokens + 4096` (capped to model context limit, default 132096).
    - If request has `reasoning_degraded == true` (or budget is explicitly suppressed to 0), completely omit `"thinking"` block from outbound Anthropic request.

#### Step 1.2: Streaming Event Adapter (`signature_delta` & `redacted_thinking`)
- **Files:** `src/upstream/provider_anthropic.c`
- **Actions:**
  - In `anthropic_stream_process_line` / `content_block_start`:
    - Handle `type: "redacted_thinking"`: mark thinking state active, emit redaction placeholder indicator.
  - In `content_block_delta`:
    - Handle `type: "signature_delta"`: capture and accumulate signature string in bridge state, preventing data loss in multi-turn conversations.
  - In `content_block_stop` & stream teardown:
    - Guarantee single start tag emission and ensure `</think>` is cleanly closed on stream EOF or abort.

#### Step 1.3: Unit Test Suite for Claude 3.7
- **Files:** `tests/unit/upstream/test_provider_reasoning.c`
- **Actions:**
  - Add test `test_claude_37_128k_budget_parsing`: verify 128,000 budget parsing and boundary clamping.
  - Add test `test_claude_37_thinking_signature_delta_stream`: verify stream parser extracts both `thinking_delta` and `signature_delta`.
  - Add test `test_claude_37_redacted_thinking`: verify `redacted_thinking` start and delta events are handled cleanly.
  - Add test `test_claude_37_degraded_suppression`: verify `"thinking"` block is suppressed when degradation occurs.

---

### Phase 2: OpenAI o1 / o3-mini Protocol Bridging & Stream Normalization

#### Step 2.1: Request Body Adaptation (`reasoning_effort` & `max_completion_tokens`)
- **Files:** `src/upstream/provider_openai.c`, `src/upstream/provider_adapter.c`
- **Actions:**
  - In `provider_openai_build`:
    - Parse `reasoning_effort` from client request or model default params. Validate against `"low"`, `"medium"`, `"high"`.
    - If targeting OpenAI o-series models or if `reasoning_effort` is set:
      - If `max_tokens` is present in request, rename/remap it to `max_completion_tokens` (OpenAI o-series requirement).
    - If degraded: strip `reasoning_effort` from request body.

#### Step 2.2: Outbound Stream Tag Emulation Bridge
- **Files:** `src/upstream/provider_openai.c`
- **Actions:**
  - In `openai_bridge_t`:
    - Add fields: `bool tag_emulation;`, `bool tag_opened;`, `bool tag_closed;`.
  - In `openai_bridge_feed` / `openai_stream_process_line`:
    - Inspect SSE chunks for `choices[0].delta.reasoning_content` and fallback `choices[0].delta.thought`.
    - If `tag_emulation == true`:
      - When first reasoning delta arrives, emit `<think>\n` into downstream `choices[0].delta.content`.
      - Stream subsequent reasoning deltas into `choices[0].delta.content`.
      - When normal `choices[0].delta.content` arrives or reasoning ends, emit `\n</think>\n\n`.
    - If `tag_emulation == false`:
      - Pass through `reasoning_content` verbatim. If only `thought` is present, normalize it to `reasoning_content`.

#### Step 2.3: Non-Streaming OpenAI Tag Emulation
- **Files:** `src/upstream/provider_openai.c`
- **Actions:**
  - In `openai_parse_chat_response`:
    - If `tag_emulation == true` and `reasoning_content` (or `thought`) is present in `choices[0].message`:
      - Synthesize formatted content: `<think>\n{reasoning}\n</think>\n\n{content}`.

#### Step 2.4: Unit Test Suite for OpenAI o1 / o3-mini
- **Files:** `tests/unit/upstream/test_provider_reasoning.c`
- **Actions:**
  - Add test `test_openai_reasoning_effort_mapping`: verify `reasoning_effort` validation and `max_completion_tokens` remapping.
  - Add test `test_openai_stream_reasoning_tag_emulation`: verify stream bridge wraps `reasoning_content` with `<think>...</think>`.
  - Add test `test_openai_stream_fallback_thought`: verify `delta.thought` normalization.
  - Add test `test_openai_sync_reasoning_tag_emulation`: verify non-streaming responses format `<think>` blocks correctly.

---

### Phase 3: Web Console Models, Keys & Groups Quota Governance

#### Step 3.1: Models Management Form & Table (`web/admin.html`)
- **Files:** `web/admin.html`
- **Actions:**
  - In `#modelModal`:
    - Add checkbox `#mFormTagEmulationDefault` (`name="tag_emulation_default"`): "默认开启 `<think>` 标签流式仿真封装".
    - Add input `#mFormPricingReasoning` in "定价策略" grid: "思考价格 ($/Mtok)".
    - Add `64K` and `128K` preset buttons for `#mFormThinkingBudget`.
  - In JS functions `openModelModal`, `submitModelModal`:
    - Populate and serialize `tag_emulation_default` and `pricing.reasoning_mtok`.
  - In `renderModelsTable`:
    - Display `🧠 Reasoning` badge with tooltip for models supporting reasoning.

#### Step 3.2: Keys & Groups Management Form & Table (`web/admin.html`)
- **Files:** `web/admin.html`
- **Actions:**
  - In `#keyModal`:
    - Add input `#kFormReasoningQuota` (`daily_reasoning_quota`): "每日推理 Token 限额 (0 为不限)".
  - In JS functions `openKeyModal`, `handleKeySubmit`:
    - Populate and serialize `daily_reasoning_quota`.
  - In `renderKeysTable`:
    - Display reasoning quota badge and `⚠️ 已降级` status badge if `key.degraded == true`.
  - In `#groupModal`:
    - Add input `#gFormDailyReasoningQuota` (`daily_reasoning_quota`): "组级每日推理配额 (0 为不限)".
  - In JS functions `openGroupModal`, `handleGroupSubmit`:
    - Populate and serialize `daily_reasoning_quota`.

---

### Phase 4: Playground Deep Reasoning Workbench & Overview Cards

#### Step 4.1: Playground Reasoning Controls (Col 1)
- **Files:** `web/admin.html`
- **Actions:**
  - Add Reasoning Format selector `#playReasoningFormat`:
    - `<option value="">🌐 默认 (跟随网关与模型策略)</option>`
    - `<option value="tag">🏷️ &lt;think&gt; 标签仿真封装</option>`
    - `<option value="raw">📦 原始模式 (reasoning_content)</option>`
  - Add dynamic Thinking Budget input/slider up to 128k (`#playThinkingBudget`).

#### Step 4.2: Collapsible Thinking Accordion & Degradation Alert Banner (Col 2)
- **Files:** `web/admin.html`
- **Actions:**
  - When rendering streaming and history assistant messages:
    - Detect `<think>` blocks or `reasoning_content`.
    - Render collapsible accordion:
      ```html
      <details class="thinking-accordion my-2 rounded-xl bg-slate-900/90 border border-purple-500/30 overflow-hidden">
        <summary class="cursor-pointer px-3 py-2 text-xs font-semibold text-purple-300 flex items-center justify-between hover:bg-slate-800/60">
          <span class="flex items-center gap-1.5"><span>🧠</span><span>思考过程 (Thinking Process)</span></span>
          <span class="text-[10px] font-mono text-purple-400/80 thinking-token-count">...</span>
        </summary>
        <div class="px-3 py-2.5 text-xs text-slate-300 font-mono whitespace-pre-wrap border-t border-slate-800/80 leading-relaxed bg-slate-950/40">...</div>
      </details>
      ```
  - When SSE stream or response headers include `X-Aigate-Reasoning-Degraded: quota_exceeded`:
    - Display amber banner at top of chat pane:
      `⚠️ 每日推理配额已超限，已自动平滑降级为标准模式生成 (X-Aigate-Reasoning-Degraded: quota_exceeded)`.

#### Step 4.3: Telemetry & Overview Integration (Col 3 & Overview Tab)
- **Files:** `web/admin.html`
- **Actions:**
  - In Col 3: Add `playStatReasoningTokens` (Reasoning Tokens count and percentage).
  - In Overview Tab: Add Reasoning Ecosystem Status Card querying `/admin/v1/reasoning/status`.

---

### Phase 5: Web UI C-Header Generation & Admin UI Unit Tests

#### Step 5.1: Build & Generated Header Verification
- **Files:** `web/admin.html`, build system
- **Actions:**
  - Compile `admin_ui_generated.h` from `web/admin.html`.

#### Step 5.2: Unit Tests in `test_admin_ui.c`
- **Files:** `tests/unit/server/test_admin_ui.c`
- **Actions:**
  - Add assertions verifying DOM presence of:
    - `mFormTagEmulationDefault`, `mFormPricingReasoning`, `kFormReasoningQuota`, `gFormDailyReasoningQuota`, `playReasoningFormat`, `thinking-accordion`, `X-Aigate-Reasoning-Degraded`.

---

### Phase 6: Comprehensive Regression Testing & Quality Gates

#### Step 6.1: Build & Compiler Diagnostics
- **Command:** `ninja -C build`
- **Requirement:** Zero compiler warnings under `-Wall -Wextra -Werror`.

#### Step 6.2: CTest Execution
- **Command:** `ctest --test-dir build --output-on-failure`
- **Requirement:** 100% pass across all test suites.

---

## Git Commit Roadmap

1. `feat(anthropic): 🧠 expand claude 3.7 128k thinking budget and sse signature bridge`
2. `feat(openai): 🔀 implement openai o1/o3-mini reasoning effort and stream tag emulation`
3. `feat(admin-ui): ⚙️ add reasoning controls to models, keys, and groups in web console`
4. `feat(playground): ⚡ upgrade playground with thinking accordion, format selector, and degraded alerts`
5. `test(reasoning): 🧪 add full test coverage for reasoning v4 and admin ui regression`
