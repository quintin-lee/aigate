# Lua Script Plugin Engine Implementation Plan

> **Date:** 2026-10-13  
> **Status:** Ready for Implementation  
> **Target Version:** aigate v0.22.0  
> **Spec Reference:** [docs/superpowers/specs/2026-10-13-lua-script-plugin-engine-design.md](file:///home/quintin/Data/source/c_cpp/aigate/docs/superpowers/specs/2026-10-13-lua-script-plugin-engine-design.md)  
> **Strict Quality Gates:** Zero warnings with `-Wall -Wextra -Werror`, 100% pass on all unit & integration tests (`ctest`), Gitmoji commits.

---

## Phase Overview

| Phase | Component / Module | Key Files | Objectives |
|:---|:---|:---|:---|
| **Phase 1** | CMake Integration & Lua Engine Foundation | `CMakeLists.txt`, `src/plugin/lua_plugin.h`, `src/plugin/lua_plugin.c` | Link Lua 5.4, build sandbox host environment, implement `aigate.*` globals, `req`/`resp` bindings, instruction limits, and test runner. |
| **Phase 2** | Database Persistence & Store Layer | `src/store/schema_sql.h`, `src/store/pg_store.h`, `src/store/pg_store.c` | Add migration v14 for `script_plugins` table, implement CRUD in `pg_store.c`, and update unit test fake store. |
| **Phase 3** | Core Gateway Pipeline & Filter Chain Hook | `src/core/aigate_core.h`, `src/core/aigate_core.c`, `src/policy/filter_chain.c`, `src/core/pipeline_chat.c` | Integrate plugin manager into gateway lifecycle, execute `on_request` in inbound pipeline with abort & rewrite, and execute `on_response` in outbound pipeline. |
| **Phase 4** | Admin REST API & Sandbox Test Endpoint | `src/server/admin_api.c` | Implement `/admin/v1/plugins` CRUD endpoints with syntax pre-validation and `/admin/v1/plugins/test` dry-run sandbox. |
| **Phase 5** | Web Console Visual Workbench | `web/admin.html` | Add "🧩 脚本插件" tab, plugin table, editor drawer with templates, and interactive debug test console. |
| **Phase 6** | Unit Testing, Sanitizers & Verification | `tests/unit/plugin/test_lua_plugin.c`, `tests/` | Comprehensive test suite covering sandbox security, timeout abort, header/body rewrites, pipeline integration, and ASan verification. |

---

## Detailed Task Breakdown

### Phase 1: CMake Integration & Lua Engine Foundation
- **Step 1.1**: Update `CMakeLists.txt` to find and link Lua 5.4 (`pkg_check_modules(LUA lua-5.4 lua5.4 lua)` with `find_package(Lua REQUIRED)` fallback).
- **Step 1.2**: Implement `src/plugin/lua_plugin.h`:
  - Definition of plugin structs (`lua_plugin_t`, `lua_plugin_mgr_t`).
  - Execution result enums (`LUA_PLUGIN_OK`, `LUA_PLUGIN_ABORT`, `LUA_PLUGIN_ERR`).
  - Manager API: `lua_plugin_mgr_create()`, `lua_plugin_mgr_destroy()`, `lua_plugin_mgr_load()`, `lua_plugin_mgr_exec_request()`, `lua_plugin_mgr_exec_response()`, `lua_plugin_test_dry_run()`.
- **Step 1.3**: Implement `src/plugin/lua_plugin.c`:
  - Lua sandbox initialization (stripping unsafe globals).
  - Instruction hook limiter (`lua_sethook`).
  - Native bindings: `aigate.log_*`, `aigate.sha256`, `aigate.json_decode`, `aigate.json_encode`, `aigate.abort`.
  - Request wrapper (`req`) and Response wrapper (`resp`) metatables and methods.
  - Safe execution wrapper with `debug.traceback`.

### Phase 2: Database Persistence & Store Layer
- **Step 2.1**: Update `src/store/schema_sql.h`:
  - Add Migration v14 creating `script_plugins` table and priority index.
- **Step 2.2**: Update `src/store/pg_store.h`:
  - Add `script_plugin_rec_t` struct and ops function pointers.
- **Step 2.3**: Update `src/store/pg_store.c`:
  - Implement `create_script_plugin`, `get_script_plugin`, `list_script_plugins`, `update_script_plugin`, `delete_script_plugin`.

### Phase 3: Core Gateway Pipeline & Filter Chain Hook
- **Step 3.1**: Update `src/core/aigate_core.h` & `src/core/aigate_core.c`:
  - Add `lua_plugin_mgr_t* plugins;` to `aigate_core`.
  - Load plugins on core startup, destroy on shutdown, implement `aigate_core_reload_plugins()`.
- **Step 3.2**: Hook into inbound filter chain:
  - If request plugin calls `aigate.abort(code, msg)`, immediately write response and return `FILTER_STOP`.
  - Apply updated model/headers/body to request context.
- **Step 3.3**: Hook into outbound pipeline:
  - In sync chat completion, execute `on_response` plugins before sending back to client.

### Phase 4: Admin REST API & Sandbox Test Endpoint
- **Step 4.1**: In `src/server/admin_api.c`:
  - `GET /admin/v1/plugins`: list all plugins.
  - `POST /admin/v1/plugins`: create plugin (pre-validates Lua syntax).
  - `GET /admin/v1/plugins/:id`: get plugin.
  - `PATCH /admin/v1/plugins/:id` & `PUT`: update plugin.
  - `DELETE /admin/v1/plugins/:id`: delete plugin.
  - `POST /admin/v1/plugins/test`: sandbox dry-run execution.

### Phase 5: Web Console Visual Workbench
- **Step 5.1**: In `web/admin.html`:
  - Add sidebar nav icon and section for "🧩 脚本插件 (Lua Plugins)".
  - Add plugin table with toggle switch and action buttons.
  - Add slide-out drawer with code editor and pre-made templates.
  - Add sandbox test runner card with interactive output view.
- **Step 5.2**: Recompile embedded header via `python3 scripts/embed_html.py`.

### Phase 6: Unit Testing, Sanitizers & Verification
- **Step 6.1**: Create `tests/unit/plugin/test_lua_plugin.c`:
  - Test syntax error rejection.
  - Test sandbox security: access to `io` / `os` blocked.
  - Test infinite loop detection.
  - Test `on_request` header, model, body modification.
  - Test `aigate.abort` interception.
  - Test `on_response` modification.
  - Test dry-run test API.
- **Step 6.2**: Add tests to `tests/unit/run_tests.c` and `tests/CMakeLists.txt`.
- **Step 6.3**: Verify 100% pass under standard build and ASan/UBSan build.
