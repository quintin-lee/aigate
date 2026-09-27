# aigate Repo Map

## Top-level directories

- `src/` — gateway source, 7 layers: `core/` (lifecycle/config/log/secrets),
  `common/` (lru/sha256), `upstream/` (model router + providers + client),
  `policy/` (auth/budget/circuit-breaker/guardrails/ratelimit/response-cache),
  `store/` (postgres/redis/usage-meter), `observe/` (event-bus/health/metrics),
  `server/` (admin API + UI + transport). `main.c` stays at `src/` root.
- `tests/unit/` — mirrors `src/` layers 1:1 (`test_<module>.c` lives in the
  same layer as `src/<layer>/<module>.c`); `run_tests.c` is the registry runner,
  `mock_upstream.c` is the shared stub.
- `tests/integration/` — pytest end-to-end suite (`test_gateway.py`,
  `test_redis_clustering.py`) plus `smoke.sh`.
- `docs/architecture/` — architecture notes; `docs/superpowers/` — specs/plans/reports.
- `web/admin.html` — ops dashboard single page, baked into the binary by
  `scripts/embed_html.py` (edit the page → re-run the script → rebuild).
- `scripts/` — build/dev helpers (`embed_html.py`).
- `schema/schema.sql` — postgres schema. `cmake/` — reserved for toolchain modules.
- `third_party/` — vendored tarballs (civetweb, hdr_histogram).

## Build (canonical)

```bash
cmake -B build -S . && cmake --build build -j && ctest --test-dir build
```

`build/` is the only documented build directory (see `.gitignore`).
