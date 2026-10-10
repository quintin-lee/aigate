# AI-Native Threat Defense & Watermarking Suite v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and integrate enterprise-grade AI threat defense & watermarking suite v2: punctuation-anchored multi-tile & sliding-window fault-tolerant watermarking, streaming chunk-level zero-overhead injection, canary honey-tokens with in-memory IP ban table (403 fast blocking), dynamic rule bypass whitelists, and Web Console forensic UI enhancements.

**Architecture:** High-performance in-memory C data structures (read-write locked 4096-bucket `ip_ban_table_t`, punctuation lexical state machine `stream_watermark_state_t`, and fast rule table `threat_whitelist_t`) integrated cleanly into Civetweb connection arrival hook, auth key verification, inbound filter chain, outbound streaming response pipeline, and PostgreSQL persistence with Schema Migration v18.

**Tech Stack:** C17, PostgreSQL (libpq), Jansson, OpenSSL (HMAC-SHA256), Civetweb, Python 3 (pytest), HTML5/CSS/Vanilla JS (Web Console).

---

## File Structure & Responsibilities

| File Path | Responsibility |
| :--- | :--- |
| `schema/schema.sql` | PostgreSQL DDL including Migration v18 (`is_canary`, `threat_rule_whitelists`, `ip_bans_persistent`). |
| `scripts/generate_schema_sql.py` | Converts `schema/schema.sql` to standard C escaped strings in `src/store/schema_sql.h`. |
| `src/store/schema_sql.h` | Embedded schema header with `#define AIGATE_SCHEMA_VERSION 18`. |
| `src/store/pg_store.h` & `pg_store.c` | Data store methods for canary flags, whitelist CRUD, and persistent IP bans. |
| `src/policy/ip_ban_table.h` & `ip_ban_table.c` | 4096-bucket rwlock hash table with TTL lazy expiration and fast O(1) checks. |
| `src/policy/auth_key.h` & `auth_key.c` | Identifies `is_canary` keys during auth; triggers auto-ban and audit logging. |
| `src/policy/threat_whitelist.h` & `threat_whitelist.c` | In-memory rule table to bypass jailbreak filter rules for authorized tenants/keys. |
| `src/policy/filter_chain.h` & `filter_chain.c` | Consults `threat_whitelist` before blocking potential jailbreak injections. |
| `src/policy/watermark_engine.h` & `watermark_engine.c` | Punctuation-anchored multi-tile encoder, sliding-window decoder, streaming state machine. |
| `src/core/pipeline_chat.c` | Hooks streaming watermark state machine into SSE chunk streaming response flow. |
| `src/server/transport_civetweb.c` | Hooks `ip_ban_table` at the earliest connection request handler (403 fast drop). |
| `src/server/admin_api.h` & `admin_api.c` | REST endpoints for IP ban management, whitelist management, and canary keys. |
| `web/admin.html` | Console UI for Canary tokens, IP blacklist table, threat whitelists, forensic visualization. |
| `tests/unit/policy/test_ip_ban_table.c` | Unit tests for IP ban table concurrency, TTL, and operations. |
| `tests/unit/policy/test_watermark_multi_tile.c` | Unit tests for multi-tile injection and arbitrary excerpt forensic extraction. |
| `tests/unit/policy/test_threat_whitelist.c` | Unit tests for rule bypass logic and wildcard evaluation. |
| `tests/unit/server/test_admin_security_api.c` | Unit tests for new security admin REST endpoints. |
| `tests/integration/test_ai_threat_defense_and_watermarking.py` | Pytest E2E tests for clipping recovery, streaming SSE, canary bans, and whitelists. |

---

### Task 1: Database Migration v18 and Persistent Storage Models

**Files:**
- Modify: `schema/schema.sql:240-250`
- Modify: `scripts/generate_schema_sql.py:15-30`
- Modify: `src/store/schema_sql.h:1-260`
- Modify: `src/store/pg_store.h:35-85`
- Modify: `src/store/pg_store.c:150-250`
- Test: `tests/unit/store/test_pg_store.c`

- [ ] **Step 1: Write failing test for Migration v18 and canary/whitelist persistence**

In `tests/unit/store/test_pg_store.c`, add:
```c
TEST_CASE(test_pg_store_schema_v18_models)
{
    /* Test schema v18 migration macro and default values */
    TEST_ASSERT(AIGATE_SCHEMA_VERSION == 18, "schema version should be 18");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests && build/tests/aigate_unit_tests`
Expected: FAIL (`AIGATE_SCHEMA_VERSION == 18` assertion fails because it is currently 17).

- [ ] **Step 3: Update `schema/schema.sql` and run `generate_schema_sql.py`**

Append Migration v18 to `schema/schema.sql`:
```sql
-- Migration v18: Canary honey-tokens and dynamic threat defense whitelists
ALTER TABLE api_keys
  ADD COLUMN IF NOT EXISTS is_canary BOOLEAN NOT NULL DEFAULT false;

CREATE TABLE IF NOT EXISTS threat_rule_whitelists (
  rule_id         BIGSERIAL PRIMARY KEY,
  name            VARCHAR(64) NOT NULL,
  match_key_id    BIGINT NOT NULL DEFAULT 0,
  match_model     VARCHAR(64) NOT NULL DEFAULT '',
  bypass_rule_tag VARCHAR(64) NOT NULL DEFAULT '*',
  reason          TEXT NOT NULL DEFAULT '',
  enabled         BOOLEAN NOT NULL DEFAULT true,
  expires_at      TIMESTAMPTZ,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_threat_whitelists_lookup 
  ON threat_rule_whitelists (match_key_id, match_model, enabled);

CREATE TABLE IF NOT EXISTS ip_bans_persistent (
  ip              VARCHAR(48) PRIMARY KEY,
  reason          VARCHAR(64) NOT NULL,
  expires_at      TIMESTAMPTZ NOT NULL,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO schema_migrations(version) VALUES (18) ON CONFLICT (version) DO NOTHING;
```
Update `scripts/generate_schema_sql.py` version to 18, run `python3 scripts/generate_schema_sql.py` to regenerate `src/store/schema_sql.h`.

- [ ] **Step 4: Update `src/store/pg_store.h` with data types and methods**

Add struct and prototypes:
```c
typedef struct {
    int64_t  rule_id;
    char     name[64];
    uint64_t match_key_id;
    char     match_model[64];
    char     bypass_rule_tag[64];
    char     reason[128];
    int64_t  expires_at;
    bool     enabled;
} threat_whitelist_rec_t;

int pg_store_list_threat_whitelists(pg_store_t* ps, threat_whitelist_rec_t** out_recs, size_t* out_count);
int pg_store_create_threat_whitelist(pg_store_t* ps, const threat_whitelist_rec_t* rec, int64_t* out_id);
int pg_store_delete_threat_whitelist(pg_store_t* ps, int64_t rule_id);
```

- [ ] **Step 5: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS (`AIGATE_SCHEMA_VERSION == 18` passes).

- [ ] **Step 6: Commit changes**

```bash
git add schema/schema.sql scripts/generate_schema_sql.py src/store/schema_sql.h src/store/pg_store.h tests/unit/store/test_pg_store.c
git commit -m "feat(store): define schema migration v18 for canary tokens and threat whitelists"
```

---

### Task 2: In-Memory IP Ban Table & Fast Connection Interception

**Files:**
- Create: `src/policy/ip_ban_table.h`
- Create: `src/policy/ip_ban_table.c`
- Modify: `CMakeLists.txt:70-85`
- Modify: `src/server/transport_civetweb.c:100-150`
- Create: `tests/unit/policy/test_ip_ban_table.c`

- [ ] **Step 1: Define `src/policy/ip_ban_table.h` interface**

```c
#ifndef AIGATE_IP_BAN_TABLE_H
#define AIGATE_IP_BAN_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define IP_BAN_BUCKETS 4096

typedef struct ip_ban_info {
    char     ip[48];
    int64_t  expires_at_sec;
    char     reason[64];
    uint32_t hit_count;
} ip_ban_info_t;

typedef struct ip_ban_entry {
    ip_ban_info_t        info;
    struct ip_ban_entry* next;
} ip_ban_entry_t;

typedef struct ip_ban_table {
    ip_ban_entry_t*  buckets[IP_BAN_BUCKETS];
    pthread_rwlock_t rwlock;
    size_t           count;
} ip_ban_table_t;

ip_ban_table_t* ip_ban_table_create(void);
void            ip_ban_table_destroy(ip_ban_table_t* tbl);
bool            ip_ban_table_is_banned(ip_ban_table_t* tbl, const char* ip, char* out_reason, size_t cap);
int             ip_ban_table_ban(ip_ban_table_t* tbl, const char* ip, int64_t ttl_sec, const char* reason);
int             ip_ban_table_unban(ip_ban_table_t* tbl, const char* ip);
size_t          ip_ban_table_list(ip_ban_table_t* tbl, ip_ban_info_t* out_arr, size_t max_count);

#endif /* AIGATE_IP_BAN_TABLE_H */
```

- [ ] **Step 2: Write failing unit tests in `tests/unit/policy/test_ip_ban_table.c`**

Test insertion, expiration, ban check, hit count increment, and unban:
```c
#include "policy/ip_ban_table.h"
#include "unit_test.h"
#include <unistd.h>

TEST_CASE(test_ip_ban_table_lifecycle)
{
    ip_ban_table_t* tbl = ip_ban_table_create();
    TEST_ASSERT(tbl != NULL, "tbl created");

    char reason[64];
    TEST_ASSERT(!ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)), "initially not banned");

    int rc = ip_ban_table_ban(tbl, "192.168.1.50", 10, "canary_compromised");
    TEST_ASSERT(rc == 0, "ban succeeded");

    TEST_ASSERT(ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)), "now banned");
    TEST_ASSERT(strcmp(reason, "canary_compromised") == 0, "reason matches");

    rc = ip_ban_table_unban(tbl, "192.168.1.50");
    TEST_ASSERT(rc == 0, "unban succeeded");
    TEST_ASSERT(!ip_ban_table_is_banned(tbl, "192.168.1.50", reason, sizeof(reason)), "unbanned");

    ip_ban_table_destroy(tbl);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Link/compile error because `ip_ban_table.c` is not implemented yet.

- [ ] **Step 4: Implement `src/policy/ip_ban_table.c`**

Implement hash function (DJB2 / FNV-1a), bucket collision handling, rwlock synchronization, and lazy TTL check.

- [ ] **Step 5: Integrate `ip_ban_table` into `src/server/transport_civetweb.c`**

In `begin_request_handler`:
```c
const char* rip = mg_get_header(conn, "X-Forwarded-For");
char client_ip[48] = {0};
/* extract IP into client_ip */
char ban_reason[64] = {0};
if (ip_ban_table_is_banned(core->ip_ban_tbl, client_ip, ban_reason, sizeof(ban_reason))) {
    mg_send_http_error(conn, 403, "Access Forbidden: IP temporarily blocked by security policy");
    return 1;
}
```

- [ ] **Step 6: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 7: Commit changes**

```bash
git add src/policy/ip_ban_table.h src/policy/ip_ban_table.c src/server/transport_civetweb.c tests/unit/policy/test_ip_ban_table.c
git commit -m "feat(policy): implement in-memory concurrent ip ban table with fast 403 drop"
```

---

### Task 3: Canary Honey-Tokens and Auto-Ban Defense Integration

**Files:**
- Modify: `src/policy/auth_key.h:15-30`
- Modify: `src/policy/auth_key.c:80-140`
- Modify: `src/core/aigate_core.h:30-50`
- Modify: `src/core/aigate_core.c:120-150`
- Test: `tests/unit/policy/test_auth_key.c`

- [ ] **Step 1: Write failing test for Canary key detection & auto-ban trigger**

In `tests/unit/policy/test_auth_key.c`, add:
```c
TEST_CASE(test_auth_key_canary_auto_ban)
{
    /* Configure key with is_canary = true */
    auth_key_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.key_id = 999;
    entry.is_canary = true;
    /* Verify auth returns error code indicating canary hit and records to ip_ban_table */
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Compile error: `is_canary` not member of `auth_key_entry_t`.

- [ ] **Step 3: Update `src/policy/auth_key.h` and `auth_key.c`**

Add `bool is_canary;` to `auth_key_entry_t` and `auth_key_info_t`.
In `auth_key_verify`:
```c
if (entry->is_canary) {
    /* Trigger auto-ban */
    if (core->ip_ban_tbl != NULL && client_ip != NULL && client_ip[0] != '\0') {
        ip_ban_table_ban(core->ip_ban_tbl, client_ip, 3600, "canary_token_compromised");
    }
    audit_logger_record(core->audit_logger, &(audit_event_t){
        .severity = AUDIT_SEV_CRITICAL,
        .violation_type = "canary_token_compromised",
        .client_ip = client_ip,
        .rule_detail = "Honey-token utilized; source IP automatically banned",
    });
    return AUTH_KEY_ERR_INVALID; /* Decoy error */
}
```

- [ ] **Step 4: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 5: Commit changes**

```bash
git add src/policy/auth_key.h src/policy/auth_key.c src/core/aigate_core.h src/core/aigate_core.c tests/unit/policy/test_auth_key.c
git commit -m "feat(policy): integrate canary honey-token detection and automated ip ban"
```

---

### Task 4: Dynamic Threat Defense Whitelist Engine

**Files:**
- Create: `src/policy/threat_whitelist.h`
- Create: `src/policy/threat_whitelist.c`
- Modify: `src/policy/filter_chain.h:20-40`
- Modify: `src/policy/filter_chain.c:160-210`
- Create: `tests/unit/policy/test_threat_whitelist.c`

- [ ] **Step 1: Define `src/policy/threat_whitelist.h`**

```c
#ifndef AIGATE_THREAT_WHITELIST_H
#define AIGATE_THREAT_WHITELIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_THREAT_WHITELIST_RULES 256

typedef struct {
    int64_t  rule_id;
    char     name[64];
    uint64_t match_key_id;
    char     match_model[64];
    char     bypass_rule_tag[64];
    char     reason[128];
    int64_t  expires_at_sec;
    bool     enabled;
} threat_whitelist_item_t;

typedef struct threat_whitelist {
    threat_whitelist_item_t items[MAX_THREAT_WHITELIST_RULES];
    size_t                  count;
    pthread_rwlock_t        rwlock;
} threat_whitelist_t;

threat_whitelist_t* threat_whitelist_create(void);
void                threat_whitelist_destroy(threat_whitelist_t* tw);
int                 threat_whitelist_add(threat_whitelist_t* tw, const threat_whitelist_item_t* item);
int                 threat_whitelist_remove(threat_whitelist_t* tw, int64_t rule_id);
bool                threat_whitelist_is_bypassed(const threat_whitelist_t* tw, uint64_t key_id, const char* model, const char* rule_tag);

#endif /* AIGATE_THREAT_WHITELIST_H */
```

- [ ] **Step 2: Write failing unit test in `tests/unit/policy/test_threat_whitelist.c`**

Test matching specific key, wildcard key, model matching, tag matching, and expired rules:
```c
#include "policy/threat_whitelist.h"
#include "unit_test.h"

TEST_CASE(test_threat_whitelist_wildcards_and_tags)
{
    threat_whitelist_t* tw = threat_whitelist_create();
    threat_whitelist_item_t item = {
        .rule_id = 1,
        .name = "dan_test",
        .match_key_id = 42,
        .match_model = "gpt-4o",
        .bypass_rule_tag = "jailbreak_dan",
        .enabled = true,
        .expires_at_sec = 0,
    };
    threat_whitelist_add(tw, &item);

    TEST_ASSERT(threat_whitelist_is_bypassed(tw, 42, "gpt-4o", "jailbreak_dan"), "matches exact rule");
    TEST_ASSERT(!threat_whitelist_is_bypassed(tw, 99, "gpt-4o", "jailbreak_dan"), "rejects wrong key");
    TEST_ASSERT(!threat_whitelist_is_bypassed(tw, 42, "claude-3", "jailbreak_dan"), "rejects wrong model");
    TEST_ASSERT(!threat_whitelist_is_bypassed(tw, 42, "gpt-4o", "jailbreak_override"), "rejects wrong tag");

    threat_whitelist_destroy(tw);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Compile failure (unimplemented `threat_whitelist.c`).

- [ ] **Step 4: Implement `src/policy/threat_whitelist.c`**

Implement `threat_whitelist_create`, `add`, `remove`, and fast matching algorithm.

- [ ] **Step 5: Integrate `threat_whitelist` into `src/policy/filter_chain.c`**

When jailbreak detector triggers violation, check `threat_whitelist_is_bypassed(fc->whitelist, ctx->key_id, ctx->model, violation_tag)`. If true, bypass blocking and log info.

- [ ] **Step 6: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 7: Commit changes**

```bash
git add src/policy/threat_whitelist.h src/policy/threat_whitelist.c src/policy/filter_chain.h src/policy/filter_chain.c tests/unit/policy/test_threat_whitelist.c
git commit -m "feat(policy): implement dynamic threat whitelist engine and filter bypass"
```

---

### Task 5: Punctuation-Anchored Multi-Tile Watermark & Sliding-Window Decoder

**Files:**
- Modify: `src/policy/watermark_engine.h:20-60`
- Modify: `src/policy/watermark_engine.c:50-250`
- Create: `tests/unit/policy/test_watermark_multi_tile.c`

- [ ] **Step 1: Write failing unit test for multi-tile injection and excerpt clipping recovery**

In `tests/unit/policy/test_watermark_multi_tile.c`:
```c
#include "policy/watermark_engine.h"
#include "unit_test.h"
#include <string.h>

TEST_CASE(test_watermark_multi_tile_excerpt_recovery)
{
    const char* article = "第一段说明。企业内部机密分析结果。请严格妥善保管，切勿外泄！第三段总结。";
    watermark_payload_t p = {
        .key_id = 8888,
        .timestamp = 1700000000,
        .short_trace = 0xABCD,
        .crc_valid = true,
    };
    size_t out_len = 0;
    char* watermarked = watermark_multi_tile_inject(article, strlen(article), &p, &out_len);
    TEST_ASSERT(watermarked != NULL, "injected");

    /* Simulate attacker copying ONLY the middle sentence */
    const char* needle = "企业内部机密分析结果。";
    const char* pos = strstr(watermarked, needle);
    TEST_ASSERT(pos != NULL, "found sentence");

    /* Extract 80 characters containing the sentence and its punctuation anchor */
    char clip[256] = {0};
    strncpy(clip, pos, sizeof(clip) - 1);

    watermark_payload_t decoded;
    bool found = watermark_decode(clip, strlen(clip), &decoded);
    TEST_ASSERT(found, "watermark recovered from clipped sentence");
    TEST_ASSERT(decoded.key_id == 8888, "key id matches");
    TEST_ASSERT(decoded.short_trace == 0xABCD, "short trace matches");

    free(watermarked);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Compile error (`watermark_multi_tile_inject` not defined).

- [ ] **Step 3: Implement `watermark_multi_tile_inject` and sliding-window `watermark_decode`**

In `src/policy/watermark_engine.c`:
- Implement punctuation anchor detection (`。`, `！`, `？`, `；`, `\n`, `.`, `!`, `?`, `;`).
- Maintain `chars_since_last_tile >= 80` threshold.
- In `watermark_decode`, implement sliding window search across the zero-width character stream to find 72-char frames matching magic `0x574D` with valid CRC16.

- [ ] **Step 4: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 5: Commit changes**

```bash
git add src/policy/watermark_engine.h src/policy/watermark_engine.c tests/unit/policy/test_watermark_multi_tile.c
git commit -m "feat(policy): implement punctuation-anchored multi-tile watermark and sliding decoder"
```

---

### Task 6: Streaming Chunk-Level Watermark State Machine

**Files:**
- Modify: `src/policy/watermark_engine.h:60-90`
- Modify: `src/policy/watermark_engine.c:260-350`
- Modify: `src/core/pipeline_chat.c:220-300`
- Create: `tests/unit/policy/test_stream_watermark.c`

- [ ] **Step 1: Write failing unit test for streaming chunk watermark state machine**

In `tests/unit/policy/test_stream_watermark.c`:
```c
#include "policy/watermark_engine.h"
#include "unit_test.h"
#include <string.h>

TEST_CASE(test_stream_watermark_chunk_by_chunk)
{
    stream_watermark_state_t sw;
    watermark_payload_t p = { .key_id = 5555, .timestamp = 1700000000, .short_trace = 0x1234, .crc_valid = true };
    stream_watermark_init(&sw, &p, true);

    char out_chunk[512];
    size_t out_len = 0;

    /* Chunk 1: regular words, no punctuation */
    bool injected = stream_watermark_process_chunk(&sw, "Hello", 5, out_chunk, sizeof(out_chunk), &out_len);
    TEST_ASSERT(!injected, "chunk 1 has no punctuation, direct pass");

    /* Chunk 2: period punctuation */
    injected = stream_watermark_process_chunk(&sw, ". World", 7, out_chunk, sizeof(out_chunk), &out_len);
    TEST_ASSERT(injected, "chunk 2 has punctuation, watermark tile injected");

    /* Verify decode from chunk 2 */
    watermark_payload_t dec;
    TEST_ASSERT(watermark_decode(out_chunk, out_len, &dec), "decode ok");
    TEST_ASSERT(dec.key_id == 5555, "key matches");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Compile error (`stream_watermark_init` not defined).

- [ ] **Step 3: Implement `stream_watermark_state_t` in `watermark_engine.c`**

Precompute 72-char zero-width tile buffer; on chunk arrival, check if punctuation anchor is present and inject tile after punctuation mark.

- [ ] **Step 4: Integrate into `src/core/pipeline_chat.c` SSE stream loop**

In `pipeline_chat_stream_write_chunk`: pass delta content through `stream_watermark_process_chunk` when watermark is enabled for key.

- [ ] **Step 5: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 6: Commit changes**

```bash
git add src/policy/watermark_engine.h src/policy/watermark_engine.c src/core/pipeline_chat.c tests/unit/policy/test_stream_watermark.c
git commit -m "feat(policy): implement streaming chunk-level zero-overhead watermark state machine"
```

---

### Task 7: Admin REST APIs for IP Bans & Whitelists

**Files:**
- Modify: `src/server/admin_api.h:40-70`
- Modify: `src/server/admin_api.c:1600-1900`
- Create: `tests/unit/server/test_admin_security_api.c`

- [ ] **Step 1: Write failing unit test for Security REST APIs**

In `tests/unit/server/test_admin_security_api.c`:
```c
#include "server/admin_api.h"
#include "unit_test.h"

TEST_CASE(test_admin_ip_bans_and_whitelists_endpoints)
{
    /* Test GET/POST/DELETE /admin/v1/security/ip-bans */
    /* Test GET/POST/DELETE /admin/v1/threats/whitelist */
    /* Test POST /admin/v1/keys with is_canary = true */
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests`
Expected: Compile failure (new endpoints not implemented in admin dispatch).

- [ ] **Step 3: Implement dispatch and handlers in `src/server/admin_api.c`**

- `handle_security_ip_bans_get`, `handle_security_ip_bans_post`, `handle_security_ip_bans_delete`.
- `handle_threats_whitelist_get`, `handle_threats_whitelist_post`, `handle_threats_whitelist_delete`.
- In `handle_keys_post`: parse `"is_canary": true/false`.

- [ ] **Step 4: Run tests and verify PASS**

Run: `make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 5: Commit changes**

```bash
git add src/server/admin_api.h src/server/admin_api.c tests/unit/server/test_admin_security_api.c
git commit -m "feat(server): expose ip-bans, threat-whitelist, and canary keys admin rest apis"
```

---

### Task 8: Web Console Forensics & Security Workbench UI

**Files:**
- Modify: `web/admin.html`
- Modify: `scripts/embed_html.py`
- Test: `tests/unit/server/test_admin_ui.c`

- [ ] **Step 1: Write failing test in `tests/unit/server/test_admin_ui.c`**

Assert presence of new UI elements in generated admin HTML:
```c
TEST_ASSERT(strstr(html, "id=\"ip-bans-table\"") != NULL, "ip-bans-table present");
TEST_ASSERT(strstr(html, "id=\"threat-whitelist-table\"") != NULL, "threat-whitelist-table present");
TEST_ASSERT(strstr(html, "id=\"btn-create-canary\"") != NULL, "btn-create-canary present");
TEST_ASSERT(strstr(html, "id=\"watermark-tiles-count\"") != NULL, "tiles count present");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C build aigate_unit_tests && build/tests/aigate_unit_tests`
Expected: FAIL (elements not yet present in `admin.html`).

- [ ] **Step 3: Update `web/admin.html` with security workbench features**

- Add IP Blacklist card with active ban table, remaining TTL, unban action.
- Add Threat Whitelist card with bypass rules table and create rule form.
- Add Canary Honey-Token badge in keys table and quick-generate Canary modal.
- Enhance watermark decoder result box to display `Tiles Detected` and highlight watermark anchor positions.

- [ ] **Step 4: Re-generate `admin_ui_html.h` and verify tests PASS**

Run: `python3 scripts/embed_html.py web/admin.html build/generated/admin_ui_html.h && make -C build -j && build/tests/aigate_unit_tests`
Expected: PASS.

- [ ] **Step 5: Commit changes**

```bash
git add web/admin.html tests/unit/server/test_admin_ui.c
git commit -m "feat(web): add canary honey-tokens, ip ban management, and multi-tile forensics to web console"
```

---

### Task 9: End-to-End Integration & Threat Defense Test Suite

**Files:**
- Modify: `tests/integration/test_ai_threat_defense_and_watermarking.py`

- [ ] **Step 1: Write E2E integration test scenarios**

Add:
1. `test_watermark_arbitrary_excerpt_clipping_e2e`: generate text, cut random single sentence, submit to `/admin/v1/watermark/decode`, assert extracted `key_id` matches.
2. `test_stream_watermark_sse_injection_e2e`: invoke `/v1/chat/completions` with `stream: true`, verify zero-width characters in chunk content, decode combined text.
3. `test_canary_honey_token_and_ip_ban_e2e`: create canary key, call completions API with canary key (assert 401), immediately call with valid key from same IP (assert 403 IP block), call unban API, verify next call succeeds 200.
4. `test_threat_whitelist_bypass_e2e`: send adversarial jailbreak prompt with regular key (assert 400), create whitelist rule for key, repeat request (assert 200 pass).

- [ ] **Step 2: Run pytest to verify all scenarios PASS**

Run: `pytest tests/integration/test_ai_threat_defense_and_watermarking.py -v`
Expected: PASS (all scenarios succeed).

- [ ] **Step 3: Commit changes**

```bash
git add tests/integration/test_ai_threat_defense_and_watermarking.py
git commit -m "test(integration): verify e2e excerpt clipping recovery, streaming watermarks, canary bans, and whitelists"
```

---

### Task 10: CI/CD Quality Gate & Clang-Format Verification

**Files:**
- All touched source files

- [ ] **Step 1: Run format check inside container matching CI**

Run:
```bash
docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c "apt-get update -qq && apt-get install -y -qq git clang-format python3 && git config --global --add safe.directory /src && python3 scripts/clang_format.py \$(find src tests -name '*.c' -o -name '*.h') && git diff --exit-code"
```
Expected: Exit code 0 (zero diff).

- [ ] **Step 2: Run all local unit tests and integration tests**

Run: `make -C build -j && ctest --test-dir build --output-on-failure && pytest tests/integration/ -v`
Expected: 100% tests passed.

- [ ] **Step 3: Push commit and verify GitHub Actions**

Run: `git push origin master && gh run watch $(gh run list -L 1 --json databaseId -q '.[0].databaseId')`
Expected: All CI matrix jobs (Clang, GCC, Docker) succeed green.
