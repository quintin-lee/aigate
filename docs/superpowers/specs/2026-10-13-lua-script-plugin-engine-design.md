# Lua Script Plugin Engine Design Specification

> **Date:** 2026-10-13  
> **Status:** Approved / In Implementation  
> **Target Version:** aigate v0.22.0  
> **Authors:** DeepMind / aigate Core Team  

---

## 1. Executive Summary & Motivation

As `aigate` grows into an enterprise AI Gateway powering high-concurrency LLM traffic, production operators need the ability to dynamically customize request and response processing without recompiling or redeploying the core C service. Common requirements include:

1. **Dynamic Prompt & Context Injection**: Prepending company disclaimers, system guidelines, or dynamic tenant context to messages.
2. **Model Mapping & Parameter Normalization**: Rewriting model identifiers (e.g. redirecting legacy `gpt-3.5-turbo` to `gpt-4o-mini`), clamping `temperature` or `max_tokens` based on tenant tiers.
3. **Custom Admission & Header Policies**: Verifying proprietary request signature tokens, injecting tenant trace headers.
4. **Response Sanitization & Transformation**: Scrubbing internal identifiers or modifying response payloads in sync mode.
5. **Zero-Downtime Hot Reloading**: Creating, updating, or toggling plugins on the fly via Admin REST API and Web Console, with instant in-memory propagation.

Following gateway industry standards established by Kong, OpenResty, and APISIX, `aigate` adopts **Lua (Lua 5.4 / LuaJIT)** as its embedded scripting language. Lua delivers microsecond-level execution latency, minimal memory footprint (< 100KB per state), and mature C ABI embedding.

---

## 2. Architecture Overview & System Flow

```
                                  Incoming Request
                                         │
                                         ▼
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ 1. Inbound Request Pipeline (src/core/pipeline_chat.c / filter_chain.c)     │
 │    - Key Auth & Rate Limiting                                               │
 │    - Lua Hook: on_request(req)                                              │
 │      * Priority-ordered execution across enabled 'request' plugins          │
 │      * Match model filter (e.g., '*' or 'gpt-*')                            │
 │      * Capabilities: req:get/set_header(), req:set_model(), req:set_body() │
 │      * Early exit: aigate.abort(status, body) -> Returns response instantly │
 └──────────────────────────────────────┬──────────────────────────────────────┘
                                        │
                                        ▼
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ 2. Upstream Dispatch & Execution (src/upstream/)                            │
 │    - Model Router selection & Circuit Breaker                               │
 │    - Upstream Provider Call (OpenAI / Anthropic / Gemini / DeepSeek)        │
 └──────────────────────────────────────┬──────────────────────────────────────┘
                                        │
                                        ▼
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ 3. Outbound Response Pipeline                                               │
 │    - Lua Hook: on_response(resp)                                            │
 │      * Priority-ordered execution across enabled 'response' plugins         │
 │      * Capabilities: resp:set_status(), resp:set_header(), resp:set_body()   │
 └──────────────────────────────────────┬──────────────────────────────────────┘
                                        │
                                        ▼
                                  Client Response
```

---

## 3. Lua Host Environment & Sandbox Security

### 3.1 Sandbox Restrictions
To prevent untrusted or buggy scripts from compromising the gateway host process:
- **Stripped Global Libraries**: Remove `os`, `io`, `package`, `coroutine`, and `debug` (preserving only `debug.traceback`).
- **Allowed Global Libraries**: `string`, `table`, `math`, `utf8`, `tostring`, `tonumber`, `type`, `pairs`, `ipairs`, `pcall`, `xpcall`.
- **Instruction Execution Quota**: Attach `lua_sethook` with a maximum instruction count (e.g. 200,000 instructions) to detect and terminate infinite loops (`while true do end`).

### 3.2 Global `aigate` Module
- `aigate.log_info(msg)`: Log message via `AIGATE_LOG_INFO`.
- `aigate.log_warn(msg)`: Log message via `AIGATE_LOG_WARN`.
- `aigate.log_error(msg)`: Log message via `AIGATE_LOG_ERROR`.
- `aigate.sha256(str)`: Compute SHA-256 hash and return lowercase hex string.
- `aigate.json_decode(str)`: Parse JSON string into Lua table.
- `aigate.json_encode(tbl)`: Serialize Lua table into JSON string.
- `aigate.abort(status_code, body_string_or_table)`: Interrupt pipeline execution and send immediate HTTP response.

### 3.3 Request Object (`req`)
- `req.method`: HTTP method string (e.g., `"POST"`).
- `req.path`: HTTP request path (e.g., `"/v1/chat/completions"`).
- `req.client_ip`: Client IP address string.
- `req.model`: Target model name.
- `req.headers`: Read-only table of request headers.
- `req:get_header(name)`: Case-insensitive header lookup.
- `req:set_header(name, value)`: Set or overwrite a request header.
- `req:get_body()`: Get raw request body string.
- `req:get_json()`: Get parsed JSON table of request body.
- `req:set_body(new_body)`: Set request body (string or table; table automatically encoded to JSON).
- `req:set_model(new_model)`: Dynamically redirect the request to another model.

### 3.4 Response Object (`resp`)
- `resp.status`: Integer HTTP status code (e.g., `200`).
- `resp.headers`: Table of response headers.
- `resp:get_header(name)`: Case-insensitive header lookup.
- `resp:set_header(name, value)`: Set or overwrite a response header.
- `resp:get_body()`: Get raw response body string.
- `resp:get_json()`: Get parsed JSON table of response body.
- `resp:set_body(new_body)`: Set response body (string or table).
- `resp:set_status(code)`: Modify HTTP status code.

---

## 4. Database Schema & Persistence

### PostgreSQL Migration v14
```sql
CREATE TABLE IF NOT EXISTS script_plugins (
    id SERIAL PRIMARY KEY,
    name VARCHAR(64) UNIQUE NOT NULL,
    phase VARCHAR(32) NOT NULL DEFAULT 'request',
    priority INT NOT NULL DEFAULT 100,
    enabled BOOLEAN NOT NULL DEFAULT true,
    match_model VARCHAR(64) NOT NULL DEFAULT '*',
    description TEXT NOT NULL DEFAULT '',
    script_code TEXT NOT NULL,
    created_at BIGINT NOT NULL,
    updated_at BIGINT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_script_plugins_enabled_priority ON script_plugins(enabled, priority);
```

---

## 5. Admin REST API Specification

| Endpoint | Method | Description |
|:---|:---|:---|
| `/admin/v1/plugins` | `GET` | List all script plugins |
| `/admin/v1/plugins` | `POST` | Create a new plugin (validates Lua syntax) |
| `/admin/v1/plugins/:id` | `GET` | Retrieve plugin by ID |
| `/admin/v1/plugins/:id` | `PUT` / `PATCH` | Update plugin configuration/script |
| `/admin/v1/plugins/:id` | `DELETE` | Delete plugin |
| `/admin/v1/plugins/test` | `POST` | Dry-run execute a plugin in an isolated sandbox with mock payload |

---

## 6. Web Console (Admin UI) Integration

- **Navigation**: New "🧩 脚本插件 (Lua Plugins)" menu item and management section.
- **Plugin Table**: Displays Name, Phase, Priority, Matched Models, Status (Enabled/Disabled), and Actions (Edit, Delete, Toggle).
- **Editor & Workbench**:
  - Full-height script editor.
  - Quick-start template selector:
    1. *Prompt Prefix Injection*: Automatically prepend system disclaimers.
    2. *Model Rewrite / Routing*: Map legacy model names to modern counterparts.
    3. *Parameter Clamp*: Enforce max temperature / max tokens ceilings.
    4. *Custom Security Header Validation*: Block requests missing proprietary headers.
  - **Interactive Dry-Run Sandbox**:
    - Input mock Request / Response JSON payload.
    - Run test against the current script.
    - View modified output, transformed headers, status code, and `aigate.log_*` console outputs.
