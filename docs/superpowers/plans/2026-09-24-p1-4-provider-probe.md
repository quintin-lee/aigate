# P1-4 Provider 健康探测 — 实施计划

Spec: `docs/superpowers/specs/2026-09-24-provider-probe-design.md` · 本文件为可逐项执行的计划,行号为写计划时快照,执行前以 grep 复核。

## 承重锚点(写计划时快照)

- registry:`src/provider_adapter.c:11-25`(`s_adapters[]` + `provider_find`,按 `supports()` 匹配)
- openai `supports`:`src/provider_openai.c:10-16`(openai/ollama/azure/deepseek/siliconflow/vllm)
- anthropic URL 规则:`src/provider_anthropic.c:31-39`(尾 `/v1` → `{ep}/messages`;尾 `/v1/` → `{ep}messages`;否则 `{ep}/v1/messages`)+ `:41-54`(x-api-key + anthropic-version:2023-06-01)
- gemini `supports`:`src/provider_gemini.c:15-19`(gemini/google);adapter URL 以 endpoint 为基址拼 `/v1beta/...`
- curl 线程 handle:`src/upstream_client.c:20-38`(`g_curl_once` + `thread_curl`,per-thread key)+ `:132-155`(协议限制/NOSIGNAL 模式)
- `mono_ns`:无公共导出(`aigate_core.c:15` static),probe 内自写
- key 解析先例:`src/model_router.c:74-105`(env:/pg: 三段语义)
- admin master 取法:`src/admin_api.c:1104-1109`(`adm->ac->router->have_master` / `master`)
- `finish_json`/`finish_error`:`src/admin_api.c:186-201`
- provider id 解析先例:`provider_patch` `src/admin_api.c:1142-1157`(`atol(rest)` + `get_provider` → 404 `not_found`)
- providers dispatch:`src/admin_api.c:1543-1550`(`rest[9]=='/'` 分支,PATCH/PUT → `provider_patch`,DELETE → `provider_delete`;**该子路由区无 POST,新增 `POST .../test` 无冲突**)
- `provider_rec_t`:`src/pg_store.h:97-107`(`name[64]`、`provider_type[32]`、`endpoint[512]`、`api_key[1024]`)
- `secret_decrypt`:`src/secrets.h:31`(`(master[32], blob, out, out_cap, out_len?)`)
- 测试注册:`tests/unit/run_tests.c` main() 内 `extern` + `test_register` 列表(CMake glob 自动纳新文件)
- mock:`tests/unit/mock_upstream.{h,c}`(`last_path` :396-399 已有;`fail_all` :375-381 固定 400-599 区间)

---

## Task 1 — mock 扩展:`mock_upstream_status`

**改** `tests/unit/mock_upstream.h`:
```c
/** @brief Force a fixed response status (400..599 or 200..399) for all
 * subsequent requests; 0 clears. Precedence: mock_status > fail_all. */
void mock_upstream_status(mock_upstream_t* mu, int status);
```

**改** `tests/unit/mock_upstream.c`:
- `struct mock_upstream`(约 :28-39)加 `int mock_status;`(mutex 保护,同 `fail_all` 模式)。
- `server_thread` `:112-125` 区:取锁块内同步拷 `int mstatus = mu->mock_status;`;响应分支改为:

```c
if (mstatus != 0) {
    int status = (mstatus >= 100 && mstatus <= 599) ? mstatus : 500;
    const char* body = "{\"mock\":\"status\"}";
    char resp[512];
    int blen = snprintf(resp, sizeof resp,
        "HTTP/1.1 %d Mock\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        status, strlen(body), body);
    write(cfd, resp, (size_t)blen);
} else if (is_fail) {
    /* 既有分支不动 */
}
```
(置于最前 → 404 也走 `mock_status`,`endpoint_unverified` 用例可用。)
- 文件尾实现 `mock_upstream_status`(取锁写 `mu->mock_status`)。

## Task 2 — 传输层 `upstream_probe`

**改** `src/upstream_client.h`(在 `upstream_stream_call` 声明后追加):
```c
/** @brief One upstream GET probe (P1-4 provider health check).
 * Reuses the per-thread curl handle; response body is discarded —
 * the verdict depends only on the status code.
 * @param hdr_name/hdr_value  auth header pair (value already final,
 *                            e.g. "Bearer sk-…"); either may be NULL
 * @param extra_hdr_name/extra_hdr_value  optional second header pair
 * @param out_status  upstream status (0 when transport failed)
 * @param out_latency_ns  optional wall duration in ns (may be NULL)
 * @return 0 transport success; -110 timeout; -502 transport failure. */
int upstream_probe(const char* url,
                   const char* hdr_name, const char* hdr_value,
                   const char* extra_hdr_name, const char* extra_hdr_value,
                   long timeout_ms,
                   int* out_status,
                   long* out_latency_ns);
```

**改** `src/upstream_client.c`(追加于 `upstream_call_ext` 之后):
```c
static size_t
discard_body(const char* buf, size_t len, void* ud)
{
    (void)buf;
    (void)ud;
    return len; /* accept, discard */
}

int
upstream_probe(const char* url,
               const char* hdr_name, const char* hdr_value,
               const char* extra_hdr_name, const char* extra_hdr_value,
               long timeout_ms,
               int* out_status,
               long* out_latency_ns)
{
    struct curl_slist* hdrs = NULL;
    int                rc = -502;
    long               http_code = 0;
    uint64_t           t0, t1;

    if (out_status != NULL) {
        *out_status = 0;
    }
    if (url == NULL || url[0] == '\0') {
        return -502;
    }
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    t0 = (uint64_t)ts0.tv_sec * 1000000000ull + (uint64_t)ts0.tv_nsec;

    pthread_once(&g_curl_once, curl_init_once);
    CURL* c = thread_curl();
    if (c == NULL) {
        return -502;
    }
    curl_easy_reset(c);

    if (hdr_name != NULL && hdr_value != NULL) {
        char hdr[1080];
        snprintf(hdr, sizeof hdr, "%s: %s", hdr_name, hdr_value);
        hdrs = curl_slist_append(hdrs, hdr);
    }
    if (extra_hdr_name != NULL && extra_hdr_value != NULL) {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "%s: %s", extra_hdr_name, extra_hdr_value);
        hdrs = curl_slist_append(hdrs, hdr);
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode cret = curl_easy_perform(c);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    t1 = (uint64_t)ts1.tv_sec * 1000000000ull + (uint64_t)ts1.tv_nsec;

    if (cret == CURLE_OK) {
        if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
            if (out_status != NULL) {
                *out_status = (int)http_code;
            }
            rc = 0;
        }
    } else if (cret == CURLE_OPERATION_TIMEDOUT) {
        rc = -110;
    } else {
        AIGATE_LOG_WARN("probe transport error: %s", curl_easy_strerror(cret));
        rc = -502;
    }

    if (out_latency_ns != NULL) {
        *out_latency_ns = (long)(t1 - t0);
    }
    curl_slist_free_all(hdrs);
    return rc;
}
```
注意:`#include <time.h>` 已在 `upstream_client.c`(计时若缺头再补)。

## Task 3 — 协议映射 `provider_probe_plan`

**改** `src/provider_adapter.h`(追加,registry 声明后):
```c
/** @brief GET /models probe plan for a provider family (P1-4).
 * Family is decided by the adapter registry (supports()), mirroring the
 * adapters' own URL/header conventions:
 *  openai family : {endpoint}/models, Authorization Bearer
 *  anthropic     : /v1-suffix rule like /messages; x-api-key + anthropic-version
 *  gemini        : {endpoint}/v1beta/models; x-goog-api-key
 * @return 0 ok; -1 when no adapter supports @p provider_type, @p endpoint
 *          is empty, or the URL would exceed 1024 chars. */
int provider_probe_plan(const char* provider_type,
                        const char* endpoint,
                        provider_probe_plan_t* out);
```
struct 定义(同 header 顶部区,`provider_find` 声明之后):
```c
typedef struct {
    char url[1024];
    char auth_header[32];
    int  bearer; /* 1 = prefix the key value with "Bearer " */
    char extra_header[32]; /* "" or "anthropic-version" */
} provider_probe_plan_t;
```
(`extra` 的 value 固定 "2023-06-01",不入 struct —— 单一事实源在 anthropic adapter `:42`。)

**改** `src/provider_adapter.c`(实现追加):
```c
int
provider_probe_plan(const char* provider_type,
                    const char* endpoint,
                    provider_probe_plan_t* out)
{
    const provider_adapter_t* adp = provider_find(provider_type);
    if (adp == NULL || endpoint == NULL || endpoint[0] == '\0' || out == NULL) {
        return -1;
    }
    char base[544]; /* endpoint[512] + NUL + slack */
    snprintf(base, sizeof base, "%s", endpoint);
    size_t elen = strlen(base);

    memset(out, 0, sizeof *out);
    int wrote = 0;
    if (strcasecmp(adp->name, "anthropic") == 0) {
        /* Mirror the /messages URL rule (provider_anthropic.c:31-39). */
        if (elen >= 3 && strcmp(base + elen - 3, "/v1") == 0) {
            wrote = snprintf(out->url, sizeof out->url, "%s/models", base);
        } else if (elen >= 4 && strcmp(base + elen - 4, "/v1/") == 0) {
            wrote = snprintf(out->url, sizeof out->url, "%smodels", base);
        } else {
            wrote = snprintf(out->url, sizeof out->url, "%s/v1/models", base);
        }
        snprintf(out->auth_header, sizeof out->auth_header, "x-api-key");
        out->bearer = 0;
        snprintf(out->extra_header, sizeof out->extra_header, "anthropic-version");
    } else if (strcasecmp(adp->name, "gemini") == 0) {
        wrote = snprintf(out->url, sizeof out->url, "%s/v1beta/models", base);
        snprintf(out->auth_header, sizeof out->auth_header, "x-goog-api-key");
        out->bearer = 0;
    } else { /* openai family: openai/ollama/azure/deepseek/siliconflow/vllm */
        wrote = snprintf(out->url, sizeof out->url, "%s/models", base);
        snprintf(out->auth_header, sizeof out->auth_header, "Authorization");
        out->bearer = 1;
    }
    if (wrote < 0 || (size_t)wrote >= sizeof out->url) {
        return -1;
    }
    return 0;
}
```
(`strcasecmp` 需 `<strings.h>`,provider_adapter.c 现只 include `<string.h>` —— 补 include。)

家族判定用 `adp->name` 而非再次 strcmp type 列表:registry 已按 `supports()` 归族,`name` 即族标签,避免 supports 名单与 plan 双写漂移。

## Task 4 — admin 端点 `POST /admin/v1/providers/{id}/test`

**改** `src/admin_api.c`:

4a. key 解析 helper(置于 `provider_patch` 之前,static;0 ok / -1 不可解析,错误类型由调用方按 `rec.api_key` 前缀定):
```c
/* P1-4: resolve a provider api_key ref for the probe.
 * "" (local, no auth) is a valid plan: out_key stays "".
 * @return 0 ok; -1 env: missing / pg: without master / secret_decrypt failed. */
static int
provider_probe_resolve_key(admin_ctx_t* adm,
                           const char*  key_ref,
                           char*        out_key,
                           size_t       out_sz)
{
    const uint8_t* master = NULL;
    int            have_master = 0;
    if (adm->ac != NULL && adm->ac->router != NULL && adm->ac->router->have_master) {
        master = adm->ac->router->master;
        have_master = 1;
    }
    if (key_ref == NULL || key_ref[0] == '\0') {
        out_key[0] = '\0';
        return 0;
    }
    if (strncmp(key_ref, "env:", 4) == 0) {
        const char* env = getenv(key_ref + 4);
        if (env == NULL || env[0] == '\0') {
            return -1;
        }
        snprintf(out_key, out_sz, "%s", env);
        return 0;
    }
    if (strncmp(key_ref, "pg:", 3) == 0) {
        if (!have_master || master == NULL) {
            return -1;
        }
        if (secret_decrypt(master, key_ref + 3, out_key, out_sz, NULL) != 0) {
            return -1;
        }
        return 0;
    }
    snprintf(out_key, out_sz, "%s", key_ref); /* plaintext (allow_plaintext_keys era) */
    return 0;
}
```
(secrets.h 引入以 `secret_encrypt`(:896 区)现状为准,缺则补 include。)

4b. handler(置于 `provider_delete` 之后;单一权威实现 —— JSON 组装在 `provider_rec_free` 之前):
```c
static int
provider_test(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid provider id");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    provider_rec_t  rec;
    if (ops->get_provider(ops->ctx, id, &rec) != 0) {
        return finish_error(status, body, len, 404, "not_found", "provider not found");
    }

    provider_probe_plan_t plan;
    if (provider_probe_plan(rec.provider_type, rec.endpoint, &plan) != 0) {
        provider_rec_free(&rec);
        return finish_error(status, body, len, 400, "probe_unsupported",
                            "no adapter supports provider_type");
    }
    char key[1080];
    if (provider_probe_resolve_key(adm, rec.api_key, key, sizeof key) != 0) {
        provider_rec_free(&rec);
        return finish_error(status, body, len, 400, "key_unresolvable",
                            "cannot resolve provider api_key ref");
    }

    char auth_value[1120];
    if (key[0] != '\0') {
        snprintf(auth_value, sizeof auth_value, "%s%s", plan.bearer ? "Bearer " : "", key);
    } else {
        auth_value[0] = '\0'; /* no-auth probe (local endpoints) */
    }

    int  us = 0;
    long lat_ns = 0;
    int  rc = upstream_probe(plan.url,
                             key[0] != '\0' ? plan.auth_header : NULL,
                             key[0] != '\0' ? auth_value : NULL,
                             plan.extra_header,
                             plan.extra_header[0] != '\0' ? "2023-06-01" : NULL,
                             10000L, &us, &lat_ns);

    const char* verdict = "unreachable";
    if (rc == 0) {
        verdict = (us >= 200 && us < 400)   ? "ok"
               : (us == 401 || us == 403)   ? "key_invalid"
               : us == 404                   ? "endpoint_unverified"
               : "upstream_error";
    } else if (rc == -110) {
        verdict = "timeout";
    }

    json_t* o = json_object();
    json_object_set_new(o, "provider", json_integer(id));
    json_object_set_new(o, "name", json_string(rec.name));
    json_object_set_new(o, "status", json_integer(us));
    json_object_set_new(o, "verdict", json_string(verdict));
    json_object_set_new(o, "latency_ms", json_real((double)lat_ns / 1000000.0));
    provider_rec_free(&rec);
    return finish_json(status, body, len, 200, o);
}
```
(探测本身成功即 admin 200;`rc != 0` 时 `us` 保持 0;`plan.extra_header` 为空串时第三参传 `""`、第四参传 NULL,`upstream_probe` 内 `value == NULL` 即跳过该 header。)

4c. dispatch(`:1543-1550` `rest[9]=='/'` 分支内,DELETE 之后追加):
```c
            if (strcmp(method, "POST") == 0 &&
                suffix_is_provider_test(rest + 10)) {
                return provider_test(adm, out_status, out_body, out_len, rest + 10);
            }
```
helper(同文件 static,置于 handler 附近):
```c
/* "7/test" — 仅 /test 后缀进入探测;其余子路径保持 404(既有 fall-through) */
static int
suffix_is_provider_test(const char* sub)
{
    char* slash = strchr(sub, '/');
    if (slash == NULL || strcmp(slash + 1, "test") != 0) {
        return 0;
    }
    size_t idlen = (size_t)(slash - sub);
    if (idlen == 0 || idlen > 18) {
        return 0;
    }
    for (size_t i = 0; i < idlen; i++) {
        if (sub[i] < '0' || sub[i] > '9') {
            return 0;
        }
    }
    return 1;
}
```
非匹配(`providers/7/foo`、`providers/abc/test`)落入既有 `:1557` 404 —— 与现有子路由 fall-through 语义一致,无需改 405:方法错误(如 GET `.../test`)同样走 404,与 providers 子路由区现状(`:1543-1550` 无 GET 分支)同构。

## Task 5 — 测试:映射 + 传输

**新建** `tests/unit/test_provider_probe.c`:
```c
#include "run_tests.h"
#include "provider_adapter.h"
#include "upstream_client.h"
#include "mock_upstream.h"
#include <string.h>

/* --- provider_probe_plan --- */
TEST_CASE(test_probe_plan_openai_family)
/* 6 个 type(openai/ollama/azure/deepseek/siliconflow/vllm)+
 * endpoint "http://api.openai.com" → url "http://api.openai.com/models",
 * auth "Authorization", bearer 1, extra "" */
{ ... }

TEST_CASE(test_probe_plan_anthropic_suffixes)
/* "http://x" → ".../v1/models"; "http://x/v1" → ".../models";
 * "http://x/v1/" → "...models"; auth "x-api-key", bearer 0, extra "anthropic-version" */
{ ... }

TEST_CASE(test_probe_plan_gemini)
/* gemini + google → "{ep}/v1beta/models", auth "x-goog-api-key" */
{ ... }

TEST_CASE(test_probe_plan_errors)
/* 未知 type "vertex" → -1;空 endpoint → -1;endpoint 2000 字节 → -1 */
{ ... }

/* --- upstream_probe 经 mock(映射+传输合成层) --- */
TEST_CASE(test_probe_transport_ok)
/* mock_upstream_start → upstream_probe(mock_base + "/models", "Authorization",
 * "Bearer x", NULL, NULL, 5000, &us, &lat) → rc 0, us 200, lat >= 0,
 * mock_last_path == "/models" */
{ ... }

TEST_CASE(test_probe_transport_key_invalid)
/* mock_upstream_status(mu, 401) → us 401(=> 可映射 key_invalid) */
{ ... }
```
注册 `run_tests.c` main():
```c
extern void test_probe_plan_openai_family(void);
extern void test_probe_plan_anthropic_suffixes(void);
extern void test_probe_plan_gemini(void);
extern void test_probe_plan_errors(void);
extern void test_probe_transport_ok(void);
extern void test_probe_transport_key_invalid(void);
```
+ 对应 `test_register("probe_plan_openai_family", test_probe_plan_openai_family);` 等 6 条(追加到列表尾部)。

**改** `tests/unit/test_upstream_client.c`:新增 1 例
```c
TEST_CASE(test_upstream_probe_get_semantics)
/* mock 200;upstream_probe(base "/models", NULL, NULL, NULL, NULL, 5000, &us, NULL)
 * → rc 0, us 200;无认证头时不发 auth(仅验证 rc/semantics,不验 header 文本 — 超范围) */
```
+ 注册。

## Task 6 — 测试:admin 端点

**改** `tests/unit/test_admin_api.c`(追加,复用既有 `setup_admin`/fake_db/`dispatch` 助手):
```c
TEST_CASE(test_admin_provider_test_ok)
/* setup_admin;fake 建 provider(type "openai", endpoint = mock_upstream_base(),
 * api_key "sk-test", allow_plaintext_keys=1);
 * admin_dispatch POST "/admin/v1/providers/{id}/test" → 200;
 * jansson 解析: verdict=="ok", status==200, latency_ms >= 0 */

TEST_CASE(test_admin_provider_test_key_invalid)
/* 同上 + mock_upstream_status(mu, 401) → verdict=="key_invalid" */

TEST_CASE(test_admin_provider_test_unverified_404)
/* mock_upstream_status(mu, 404) → verdict=="endpoint_unverified" */

TEST_CASE(test_admin_provider_test_missing_key_env)
/* provider api_key "env:AIGATE_NOPE_PROBE"(unset)→ 400 key_unresolvable */

TEST_CASE(test_admin_provider_test_unknown_type)
/* provider_type "vertex" → 400 probe_unsupported */

TEST_CASE(test_admin_provider_test_not_found)
/* 不存在的 id → 404 */
```
pg: 无 master 用例:setup_admin 的 `aigate_core_init(..., NULL, ...)` 即无 master(fake router 保持 `have_master==0`)→ provider api_key `pg:zzz` → 400 `key_unresolvable`(并入 `missing_key_env` 同组或独立例,执行时定)。

## Task 7 — 构建、验证、提交

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
cd build && ctest -R unit --output-on-failure
cmake -S . -B build-asan -DAIGATE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j && (cd build-asan && ctest -R unit)
```
- 零告警(-Wall -Wextra -Werror);unit 全绿(含既有 providers CRUD 不回归);ASan run 全绿(重点:mock socket 线程 + curl 线程 handle 复用无 UAF/leak——leak 只查本例新增路径)。
- Gitmoji(拆 3 提交):
  - `feat(upstream): add upstream_probe GET health-check transport (P1-4)` — Task 1+2+5(mock+probe+映射测试)
  - `feat(provider): add provider_probe_plan family mapping (P1-4)` — Task 3
  - `feat(admin): add POST /admin/v1/providers/{id}/test probe endpoint (P1-4)` — Task 4+6

## 风险与执行时决策点

| # | 风险/未定 | 应对 |
|---|---|---|
| R1 | `discard_body` 回调名与既有 `append_body` 无冲突;若 `upstream_client.c` 已有同名再调整 | 执行前 grep `discard` |
| R2 | 行号漂移(已多轮提交) | 每个 Task 动手前按函数名 grep 复核 |
| R3 | `strcasecmp` 需 `<strings.h>` | Task 3 已注 |
| R4 | provider_test JSON 组装在 `provider_rec_free` 之前(Task 4b 笔误警示) | 按 4b 末段最终代码实现 |
| R5 | mock `mock_status` 的 status 上限/下限(100-599)与 fail_all(400-599)并存时优先级 | mock_status 优先(文档注明) |
| R6 | `AIGATE_LOG_*` 宏在 upstream_client.c 已有使用,probe 直接复用 | — |
