# Core Algorithms and Concurrency Comments Design

**Date:** 2026-10-01  
**Status:** Approved  
**Topic:** In-depth algorithmic, state machine, and concurrency inline commentary across 4 critical subsystems

---

## 1. Motivation and Goals

The previous Doxygen alignment established a zero-warning baseline for all header declarations, struct members, and public symbols across the codebase. However, several critical subsystems contain complex algorithmic logic, state machine transitions, and multi-threaded race conditions whose internal invariants and execution flow require explicit, step-by-step inline documentation.

### Target Subsystems
1. **Hedged Concurrent Requests Engine** (`src/upstream/upstream_hedged.c`)
2. **Adaptive Latency-Aware Routing** (`src/upstream/model_router.c`)
3. **Aho-Corasick Multipattern Matching Engine** (`src/policy/guardrails.c`)
4. **Streaming SSE Pipeline & TTFT Metrics Extraction** (`src/core/pipeline_chat.c` & `src/core/aigate_core.c`)

### Key Principles
- **Structured Steps**: Use numbered stages (`/* Step 1: ... */`, `/* Step 2: ... */`) for complex control flows.
- **Explicit Invariants**: Explain concurrency invariants (mutex scope, condition variable signaling, cancellation propagation, reference-counted lifecycle) and algorithmic invariants (DFA state transition, EWMA decay formula, roulette wheel probability).
- **Zero Runtime Logic Modification**: Changes are strictly comments and whitespace. Zero functional alterations to code or control flow.
- **Maintain Doxygen & Style Cleanliness**: 0 Doxygen warnings, clang-format compliance, and pure English prose.

---

## 2. Detailed Technical Scope

### 2.1 Hedged Concurrent Requests Engine (`src/upstream/upstream_hedged.c`)
- **`worker_thread`**:
  - *Step 1: Setup & Request Configuration*: Document libcurl handle configuration, timeouts, URL/key setup, and headers copy.
  - *Step 2: Network I/O & Cooperative Cancellation Check*: Document how cancellation flags (`ctrl->cancel_flags[my_idx]`) are polled during transport callbacks to abort losing branches early.
  - *Step 3: Mutex Acquisition & Invariant Check*: Document the critical section condition: the first worker to finish successfully (or any worker if competitor failed) claims `winning_idx`.
  - *Step 4: Winner Arbitration & Peer Cancellation*: Set `winning_idx`, signal cancellation flag to competitor (`ctrl->cancel_flags[other] = 1`), and wake coordinator via `pthread_cond_signal(&ctrl->cond)`.
  - *Step 5: Reference-Counted Lifecycle & Safe Destruction*: Explain how atomic `ctrl->ref_count` decrement ensures that whichever thread (coordinator or either worker) drops the last reference safely frees the shared control block.
- **`upstream_call_hedged`**:
  - *Phase 1: Validation & Primary Dispatch*: Validate arguments, allocate control block (`ref_count = 2` or `3`), and spawn primary worker thread.
  - *Phase 2: Hedging Delay Window*: Explain time calculation based on P95 latency and `budget_pct` admission check.
  - *Phase 3: Speculative Secondary Dispatch*: If primary did not complete within delay window and budget permits, spawn secondary worker thread.
  - *Phase 4: Coordinated Wait on Condition Variable*: Wait on `pthread_cond_wait(&ctrl->cond, &ctrl->mutex)` until a winning response or terminal failure arrives.
  - *Phase 5: Result Extraction & Cleanup*: Copy winner body/status, update metrics and latency tracker, and decrement coordinator's reference count.

### 2.2 Adaptive Latency Routing & Dynamic Weighting (`src/upstream/model_router.c`)
- **`route_select_target`**:
  - *Step 1: Priority Partitioning & Normalization*: Sort and deduplicate distinct priority tiers ascending (priority 0 = primary tier).
  - *Step 2: Circuit Breaker Snapshot Invariant*: Explain why `allowed[i]` is evaluated once per target up front (avoiding side-effect mutation of `HALF_OPEN` probe states across multiple tier iterations).
  - *Step 3: Policy Selection & Tier Evaluation*:
    - **`latency_p95`**: Document retrieval of historical P95 millisecond samples from `latency_tracker`, ascending sort (fastest target first), and tie-breaking.
    - **`dynamic_weighted`**: Document dynamic weight derivation:
      $$W_i = \max\left(1, \frac{1000}{\text{EWMA}_i + 10}\right)$$
      Explain the `+ 10` dampening constant preventing division-by-zero on micro-latencies. Document roulette wheel cumulative sum and atomic counter pick.
    - **`round_robin` / `weighted`**: Document relaxed atomic counter increment preventing cross-core cache line contention.
  - *Step 4: All-Tripped Fallback*: Document recovery behavior when all endpoints in all tiers are tripped by circuit breaker.

### 2.3 Aho-Corasick Multipattern Matching Engine (`src/policy/guardrails.c`)
- **`ac_trie_insert`**:
  - *Step 1: Byte-Level Trie Traversal*: Decompose UTF-8 byte sequences into `unsigned char` transitions over 256-way branching nodes.
  - *Step 2: Dynamic Capacity Growth*: Document 2x reallocation factor of `trie->nodes`.
  - *Step 3: Terminal Node Keyword Attachment*: Attach keyword string copy at leaf node.
- **`ac_trie_build_failure_links`**:
  - *Step 1: Root Self-Loops & Level-1 Seeding*: Root node missing transitions self-loop to state 0; level-1 children set `fail = 0` and seed BFS queue.
  - *Step 2: BFS Queue Processing*: Breadth-first traversal computing $fail(u) = next[fail(r)][c]$.
  - *Step 3: Output Link Compression (Keyword Inheritance)*: If failure state has a matched keyword, inherit it onto current state, eliminating runtime failure traversal loops.
  - *Step 4: DFA Deterministic State Compression*: If child transition is missing (`next[r][c] == -1`), compress it to point directly to `next[fail_state][c]`. Guarantees strict $O(1)$ single-table-lookup transitions per text byte during search.
- **`ac_trie_search` & Inbound Inspection**:
  - *Step 1: $O(|text|)$ Deterministic Text Scan*: Scan characters with zero failure loops.
  - *Step 2: Blocklist vs. Exemption Dual-Trie Arbitration*: Cross-reference hits against exemption trie before final decision.
  - *Step 3: PII Masking Pipeline*: Apply regex filters for API keys, phone numbers, email addresses, and ID cards.

### 2.4 Streaming SSE Pipeline & TTFT Extraction (`src/core/pipeline_chat.c` & `src/core/aigate_core.c`)
- **`stream_feed_wrapper_fn` (`src/core/pipeline_chat.c`)**:
  - *Step 1: First-Chunk Detection*: Atomically detect first non-empty chunk via `first_chunk_recorded`.
  - *Step 2: Monotonic Latency Recording*: Compute time-to-first-token $\Delta t = \text{mono\_ns}() - t_0$ and feed to `latency_tracker`.
  - *Step 3: Zero-Copy Passthrough*: Direct invocation of `real_feed` without buffering delay.
- **`stream_cache_acc_write` (`src/core/aigate_core.c`)**:
  - *Step 1: Downstream Forwarding*: Immediate write to client response sink.
  - *Step 2: Cross-Frame Chunk Line Slicing*: Scan for newline using `memchr`, preserving partial segments in dynamic `line_buf`.
  - *Step 3: SSE Protocol Parsing & Accumulation*: Detect `data: [DONE]`, parse JSON payload, extract `id`, `created`, and `choices[0].delta.content`.
  - *Step 4: Memory Safeguard*: Enforce 512 KiB limit, setting `overflow = true` to abort caching on oversized streams while continuing passthrough.

---

## 3. Verification and Validation Gates

1. **Doxygen Gate**: `doxygen Doxyfile 2>&1 | grep "warning:"` must output 0 lines.
2. **Comment-Only Diff Check**: Verify via `git diff` that no binary code or logic branches are modified.
3. **English Consistency**: `grep -rP '[\x{4e00}-\x{9fff}]' src/` must return 0 lines.
4. **Clean Build**: `cmake --build build -j` must succeed with zero warnings.
5. **Code Style Gate**: `git diff --check` and `ninja format` must pass cleanly.
6. **Full Test Suite**: `ctest --test-dir build --output-on-failure` must pass 6/6 tests (100%).
