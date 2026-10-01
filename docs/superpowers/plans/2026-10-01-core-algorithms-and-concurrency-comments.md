# Core Algorithms and Concurrency Comments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deepen internal algorithmic, state machine, and concurrency inline documentation across 4 core subsystems (`upstream_hedged.c`, `model_router.c`, `guardrails.c`, and `pipeline_chat.c`/`aigate_core.c`), while maintaining zero logic changes, zero Doxygen warnings, and 100% test pass rate.

**Architecture:** Pure documentation and code comment additions. Organized into 4 targeted editing tasks followed by a unified verification gate.

**Tech Stack:** C11, Doxygen, Clang-Format, CMake, CTest.

**Spec:** [`docs/superpowers/specs/2026-10-01-core-algorithms-and-concurrency-comments-design.md`](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-01-core-algorithms-and-concurrency-comments-design.md)

---

### Task 1: Hedged Concurrent Requests Concurrency Comments (`src/upstream/upstream_hedged.c`)

**Files:**
- Modify: `src/upstream/upstream_hedged.c`

- [ ] **Step 1: Document `worker_thread` Internal Control Flow**
  - Add Step 1-5 comments:
    - Step 1: Curl setup, headers duplication, timeout and credentials binding.
    - Step 2: Transport I/O loop with cooperative cancellation polling via `ctrl->cancel_flags[my_idx]`.
    - Step 3: Critical section entry (`pthread_mutex_lock`), explaining the first-finisher invariant.
    - Step 4: Race decision: setting `winning_idx`, setting competitor cancel flag `ctrl->cancel_flags[other] = 1`, and signaling coordinator with `pthread_cond_signal`.
    - Step 5: Reference-counted deallocation pattern (`ref_count` decrement and free condition).

- [ ] **Step 2: Document `upstream_call_hedged` Coordination Flow**
  - Add Phase 1-5 comments:
    - Phase 1: Input validation, control block initialization, and primary thread dispatch.
    - Phase 2: Speculative delay calculation and hedge budget gating check via `latency_tracker_hedge_admitted`.
    - Phase 3: Secondary backup dispatch on delay expiration.
    - Phase 4: Synchronized outcome wait on `pthread_cond_wait`.
    - Phase 5: Winner extraction, metrics and latency recording, and coordinator reference drop.

- [ ] **Step 3: Verify Zero Doxygen Warnings in upstream**
  Run: `doxygen Doxyfile 2>&1 | grep "src/upstream/"`
  Expected: Empty output.

---

### Task 2: Adaptive Latency Routing & Dynamic Weighting Comments (`src/upstream/model_router.c`)

**Files:**
- Modify: `src/upstream/model_router.c`

- [ ] **Step 1: Document `route_select_target` Priority Partitioning & Filtering**
  - Document priority tier gathering and ascending insertion sort.
  - Document circuit breaker pre-filtering snapshot invariant (preventing repeated calls from mutating `HALF_OPEN` state).
  - Document all-tripped recovery fallback searching for earliest `open_until`.

- [ ] **Step 2: Document Routing Policy Implementations**
  - Document `round_robin` atomic counter modulo without locks.
  - Document `latency_p95` algorithm: sample retrieval from tracker, sorting ascending, and tie-breaking.
  - Document `dynamic_weighted` algorithm: mathematical formula $W_i = \max(1, 1000 / (\text{EWMA}_i + 10))$, dampening term rationale, roulette wheel selection, and descending fallback sort.

- [ ] **Step 3: Verify Zero Doxygen Warnings in model_router**
  Run: `doxygen Doxyfile 2>&1 | grep "model_router"`
  Expected: Empty output.

---

### Task 3: Aho-Corasick Multipattern Matching Engine Comments (`src/policy/guardrails.c`)

**Files:**
- Modify: `src/policy/guardrails.c`

- [ ] **Step 1: Document `ac_trie_insert` Trie Expansion**
  - Document UTF-8 byte stream traversal, dynamic doubling reallocation of node table, and keyword attachment at leaf nodes.

- [ ] **Step 2: Document `ac_trie_build_failure_links` BFS & DFA Compression**
  - Document root self-loop initialization and level-1 children seed insertion.
  - Document BFS queue traversal and failure link construction ($fail(u) = next[fail(r)][c]$).
  - Document output link compression propagating matched keywords from failure ancestor to current state.
  - Document DFA transition compression: redirecting missing edges directly to failure state's transition, achieving strict $O(1)$ per-character lookup.

- [ ] **Step 3: Document `ac_trie_search` & Inbound Inspection**
  - Document $O(|text|)$ deterministic search loop without backtrack loops.
  - Document dual-trie arbitration (blocklist hit checking exemption trie).

- [ ] **Step 4: Verify Zero Doxygen Warnings in guardrails**
  Run: `doxygen Doxyfile 2>&1 | grep "src/policy/"`
  Expected: Empty output.

---

### Task 4: Streaming SSE Pipeline & TTFT Metrics Comments (`src/core/pipeline_chat.c` & `src/core/aigate_core.c`)

**Files:**
- Modify: `src/core/pipeline_chat.c`
- Modify: `src/core/aigate_core.c`

- [ ] **Step 1: Document TTFT Extraction in `stream_feed_wrapper_fn`**
  - Document first-chunk detection using `first_chunk_recorded` flag.
  - Document elapsed time calculation $\Delta t = \text{mono\_ns}() - t_0$ and feed to `latency_tracker_record`.
  - Document immediate zero-copy downstream pass to `real_feed`.

- [ ] **Step 2: Document `stream_cache_acc_write` in `aigate_core.c`**
  - Document immediate client stream forwarding.
  - Document newline detection (`memchr`) and dynamic line assembly across TCP chunk boundaries.
  - Document SSE prefix handling (`data: [DONE]`), token delta aggregation, and 512 KiB buffer overflow safeguard.

- [ ] **Step 3: Verify Zero Doxygen Warnings in core**
  Run: `doxygen Doxyfile 2>&1 | grep "src/core/"`
  Expected: Empty output.

---

### Task 5: Comprehensive Verification, Formatting, and Git Commit

- [ ] **Step 1: Check Doxygen Zero-Warning Baseline**
  Run: `doxygen Doxyfile 2>&1 | grep "warning:"`
  Expected: 0 lines output.

- [ ] **Step 2: Check English Purity in C Source**
  Run: `grep -rP '[\x{4e00}-\x{9fff}]' src/`
  Expected: 0 matches.

- [ ] **Step 3: Machine-Check Comment-Only Diff**
  Run: `git diff -U0 src/ | grep '^[+-]' | grep -vE '^[+-]{3}' | grep -vE '^[+-]\s*(\*|/\*|//|\*/|$)'`
  Expected: Empty output.

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
  Run: `git add src/ docs/superpowers/plans/2026-10-01-core-algorithms-and-concurrency-comments.md && git commit -m "docs: 📝 add detailed algorithmic and concurrency inline comments across core subsystems"`
