# Provider 健康探测(P1-4)设计

日期:2026-09-24 · 场景:内部自用(单组织)· 立项依据:`docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md` §5(候选池「价值 5 + 改动面最小 + 零依赖」,下一立项)

## 0. 目标与范围

- **目标**:上游 key 失效时,运维通过 admin 端点一键定位(401/403 → key_invalid),而非等真实流量 502。
- **端点**:`POST /admin/v1/providers/{id}/test`(单 provider;鉴权 = 既有 admin token 族 + IP 锁留,走 `admin_dispatch` 现有前置)。
- **语义**:按需单级探测 —— GET `{endpoint}/models`(按 provider 家族映射 URL/鉴权头),零 token 成本。
- **范围外(YAGNI)**:
  - 不做自动后台探测、不做结果落库(无 `last_test_at` 列,零 schema 变更);
  - 不做全量端点(`GET /admin/v1/providers/test`),逐家调用单端点即可;
  - 探测结果不进 `/metrics`(可作后续小项,不在本 spec);
  - 不做超时可配(固定 10s,非热路径);
  - 不实现两级探测(/models 404 后升级最小 chat)——404 记 `endpoint_unverified` 不作失败结论。

## 1. 三层结构(方案 A)

### 1.1 协议映射(纯函数,零网络)`provider_adapter.{h,c}`

```c
typedef struct {
    char url[1024];        /* 完整 GET URL */
    char auth_header[32];  /* "Authorization" | "x-api-key" | "x-goog-api-key" */
    int  bearer;           /* 1 = 值须前缀 "Bearer " */
    char extra_header[32]; /* 可空;"anthropic-version"(值固定 "2023-06-01") */
} provider_probe_plan_t;

/** @brief Map a provider_type + endpoint to a GET /models probe plan.
 * @return 0 ok; -1 = no adapter supports @p provider_type or @p endpoint
 *         is empty/truncating beyond 1024. */
int provider_probe_plan(const char* provider_type, const char* endpoint,
                        provider_probe_plan_t* out);
```

家族判定挂既有 `provider_find`(`provider_adapter.c:14-25`):

| 家族(`supports()` 现值) | 探测 URL | 鉴权头 | 附加头 |
|---|---|---|---|
| openai/ollama/azure/deepseek/siliconflow/vllm(`provider_openai.c:11-16`) | `{endpoint}/models` | `Authorization`,`bearer=1` | — |
| anthropic(`provider_anthropic.c:15-19`) | 镜像 `/messages` URL 规则(`provider_anthropic.c:30-40`):`endpoint 尾 "/v1"` → `{ep}/models`;尾 `"/v1/"` → `{ep}models`;否则 `{ep}/v1/models` | `x-api-key`,`bearer=0` | `anthropic-version: 2023-06-01` |
| gemini/google(`provider_gemini.c:15-19`) | `{endpoint}/v1beta/models`(镜像 adapter 的 generateContent 基址语义 `provider_gemini.c:79`) | `x-goog-api-key`,`bearer=0` | — |
| 其余(无 adapter) | — | 返回 -1 | admin 层 400 `probe_unsupported` |

实现约束:URL 用 `snprintf(plan->url, sizeof plan->url, ...)`;截断(返回 ≥ sizeof)→ 返回 -1;`endpoint` 空 → -1。

### 1.2 传输(纯 curl)`upstream_client.{c,h}`

```c
/** @brief One upstream GET probe, reusing the per-thread curl handle.
 * @return 0 = transport success (upstream status in *out_status, even 4xx/5xx);
 *         -110 timeout; -502 transport failure (in which case *out_status = 0).
 * @p out_latency_ns optional (NULL ok): wall duration of the transfer. */
int upstream_probe(const char* url,
                  const char* hdr_name, const char* hdr_value,
                  const char* extra_hdr_name, const char* extra_hdr_value,
                  long        timeout_ms,
                  int*        out_status,
                  long*       out_latency_ns);
```

镜像 `upstream_call_ext` 的 curl 配置(`upstream_client.c:132-155`):per-thread handle(`pthread_key` :20-38)、`CURLOPT_PROTOCOLS` http/https 限制(:147-152)、`CURLOPT_NOSIGNAL`(:155)、`CURLOPT_TIMEOUT_MS`(默认 60000,此处恒传 10000)。差异:

- `CURLOPT_HTTPGET = 1L`,无 `CURLOPT_POST`/`POSTFIELDS`;
- 请求头:`hdr_name: hdr_value`(+ `Bearer ` 前缀由调用方拼好,`hdr_value` 即最终值),`extra_hdr_name` 非空时追加;
- 不捕获响应体(无 WRITEFUNCTION 数据,写函数丢弃 —— 或 `CURLOPT_NOBODY` 之外的空写入;verdict 只依赖 status)。

计时:函数入口/出口 `CLOCK_MONOTONIC`(同 `aigate_core.c:15-21` `mono_ns`)。

### 1.3 Admin handler `admin_api.c`

`provider_test(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* id_str)` + dispatch 分支:

- 位置:providers 分支(`admin_api.c:1545-1559` 区)内,`strncmp(rest, "providers/", 10) == 0` 且末段为 `"/{id}/test"` 且 method = POST(错 method → 405,沿用现有 providers 子路由模式)。
- 处理序(每步失败短路):
  1. `get_provider` 失败 → 404 `provider_not_found`;
  2. `provider_probe_plan(rec.provider_type, rec.endpoint, &plan)` == -1 → 400 `probe_unsupported`(detail 含 provider_type);
  3. key 解析(`rec.api_key`,`admin_api.c:1105-1109` 现成 master 取法):
     - 空 → 400 `key_missing`;
     - `env:<NAME>`:getenv 缺失/空 → 400 `key_unresolvable`;
     - `pg:<ref>`:需 `adm->ac->router->have_master`(无 → 400 `key_unresolvable`,"set AIGATE_MASTER_KEY");`secret_decrypt`(`src/secrets.h:31`)失败 → 400 `key_unresolvable`;
     - 明文直用(存量明文 key,与 provider 管理面既有语义一致);
  4. `upstream_probe(plan.url, plan.auth_header, value, plan.extra_header ?: NULL, plan.extra_header 非空 ? "2023-06-01" : NULL, 10000, &status, &lat_ns)`;
  5. verdict 五分类:

| verdict | 触发 |
|---|---|
| `ok` | rc==0 且 status 2xx/3xx |
| `key_invalid` | rc==0 且 status 401/403 |
| `endpoint_unverified` | rc==0 且 status 404 |
| `upstream_error` | rc==0 且 status ≥ 500(其它 4xx 归 `upstream_error`,detail 带 status) |
| `timeout` | rc == -110 |
| `unreachable` | rc == -502 |

响应(探测本身总为 admin 200,verdict 在 body 内 —— 上游故障不当 admin 5xx):

```json
{ "provider": 7, "name": "my-openai", "status": 200,
  "verdict": "ok", "latency_ms": 42.3 }
```

`status` = 上游状态码(传输失败为 0);`latency_ms` = `lat_ns / 1000000.0`。

安全面:探测目标 = 管理员创建 provider 时已配置的 endpoint(创建本身即可指向任意端点,probe 不引入新网络原语);key 明文只在内存 + curl header,不进日志(沿用 `mask_api_key` 原则,`:850` 区);FOLLOWLOCATION 沿用 `upstream_client.c` 既有 http/https 限制。

## 2. 数据流

```
client → admin_dispatch(token 族 + IP 锁留,既有前置)
  → provider_test:
      get_provider(rec)
      → provider_probe_plan(type, endpoint, plan)      [纯映射]
      → key 解析(明文 | env: getenv | pg: secret_decrypt(master))
      → upstream_probe(plan, key, 10s)                 [curl GET,复用线程 handle]
      → verdict 五分类 → 200 JSON
```

## 3. 测试

| 层 | 用例 | 文件 |
|---|---|---|
| 映射单测 | 6 个 openai 家族 type 各 1 + 默认 family 判定;anthropic 3 种 endpoint 尾(`http://x`、`http://x/v1`、`http://x/v1/`)→ 3 种 URL;gemini;未知 type → -1;空 endpoint → -1;超长 endpoint 截断 → -1 | `tests/unit/test_provider_probe.c`(新建,`run_tests.c` 注册) |
| 传输单测 | mock 200 → rc 0 + status 200 + latency ≥ 0;`mock_upstream_status(mu,401)` → verdict 可判;mock 已 stop 的 base → -502 unreachable;GET 语义:mock `last_path == "/models"`(扩展 `mock_upstream.{c,h}`:加 `void mock_upstream_status(mock_upstream_t*, int)` 固定状态码模式,同 `fail_all` 模式 :375-381) | `test_provider_probe.c` + `tests/unit/test_upstream_client.c`(加 1 例) |
| 端点单测 | `ok`/`key_invalid`(mock 401)/`endpoint_unverified`(mock 404)/ 空 key 400 `key_missing` / `env:` 缺失 400 / `pg:` 无 master 400 / 未知 provider_type 400 `probe_unsupported` / GET 方法 405 / 不存在 provider 404 | `tests/unit/test_admin_api.c`(新建 provider 用明文 key + fake `allow_plaintext_keys=1`;pg: 用例用 fake + 无 router master 的 ctx) |

超时(-110)不做确定性测试(需慢 mock,YAGNI;plan 阶段记录,不进用例)。

## 4. 验收

- `cmake --build build -j` 零告警(-Wall -Wextra -Werror)+ `ctest -R unit` 全绿;
- 集成 smoke(真库 + mock 上游不可达场景):`POST /admin/v1/providers/{id}/test` 返回 `unreachable` verdict(验证端到端接线);
- 既有 providers CRUD 测试不回归;
- Gitmoji 分模块提交:`feat(upstream)` probe、`feat(provider)` plan、`feat(admin)` 端点、`test` 各层。

## 5. 不做(记录理由)

- 全量探测端点:逐家调用单端点即可,省 ~30 行 + 无并发语义;
- 后台自动探测/降级:与 circuit_breaker 动态熔断职责重叠,等「无流量 provider 也要发现 key 失效」的真实需求再评估;
- 两级探测:复杂度最高、收益仅在「网关不暴露 /models」的少数场景,verdict `endpoint_unverified` 已把该信息交给运维判断;
- 超时可配:非热路径,10s 足够,`AIGATE_MAX_*` 一族已有同类可配项,探测暂不需要。
