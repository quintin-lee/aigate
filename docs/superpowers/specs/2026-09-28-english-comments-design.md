# English Comments Design

**Goal:** Translate all Chinese code comments to English; behavior unchanged.

**Scope (measured 2026-09-28):** ~1998 CJK lines in 72 files.
- `src/`: 46 files, 872 lines (common 23, core 92, observe 121, server 97, policy 147, store 224, upstream 165, main.c 3)
- `tests/`: 25 files, 172 lines (test_admin_api.c 36, test_pg_store.c 35, test_aigate_core.c 18 largest)
- `web/admin.html`: 1 file, 954 lines
- Excluded: `tests/**/__pycache__/*.pyc` binaries, string literals, identifiers.

**Approach:** Option A — batched translation with independent commits (9 batches: 7 src layers + tests + web).

**§1 Scope:** All `//`, `/* */`, `/** */`, `/**< */` comment text and Doxygen `@brief/@param/@return` descriptions. Untouched: string literals (logs, errors, API output), identifiers, `__pycache__`.

**§2 Format:** Keep comment structure and Doxygen markers; translate prose only. Unified terms: circuit breaker, guardrails, quota, budget, probe; states closed/open/half-open in English.

**§3 Verification (per batch):** Four standard checks (zero-warning build, ctest 6/6, comment-only diff machine check, doxygen 0 warnings) plus translation check: zero CJK chars in source files (`grep -rP '[\x{4e00}-\x{9fff}]'` empty, `__pycache__` excluded). Commit only batch paths (`docs: translate <batch> comments to English`).
