/** @file pg_store.h
 *  @brief PostgreSQL persistence layer: schema migration + an ops table so
 *  that every caller (auth cache, admin API, flush worker) talks to a
 *  uniform interface that unit tests can fake (spec section 3).
 *
 *  Canonical record types are defined here and shared by all modules.
 */

/**
 * @defgroup group_store 存储层
 * @brief 存储：Postgres、Redis、用量计量。
 */
#ifndef AIGATE_PG_STORE_H
#define AIGATE_PG_STORE_H

#include <stdint.h>
#include <time.h>

/** @brief Client API key record (api_keys row; allowed_models is a copy). */
typedef struct key_rec {
    long   key_id; /**< 主键 */
    char   key_hash[65]; /**< 64 lowercase hex + NUL */
    char   name[128]; /**< 名称 */
    char** allowed_models; /**< NUL-terminated-ish: exactly n_allowed entries */
    int    n_allowed; /**< 0 = all models allowed */
    int    rate_qps; /**< 0 = unlimited */
    long   daily_token_quota; /**< 0 = unlimited */
    time_t expires_at; /**< 过期时间戳 */
    int    has_expiry; /**< 是否设置过期（1 生效） */
    int    revoked; /**< 吊销标记（1 已吊销） */
    long   group_id; /**< 0 = ungrouped */
    int    guardrails_enabled; /**< 1 = enabled (default), 0 = disabled */
    double monthly_cost_budget; /**< 0.0 = unlimited */
    long   monthly_token_budget; /**< 0 = unlimited */
} key_rec_t;

/** @brief 单模型多目标上限。 */
#define MAX_TARGETS_PER_MODEL 8

/** @brief 单个上游目标：供应商、基址、密钥引用与负载权重。 */
typedef struct upstream_target {
    char provider[32]; /**< 供应商类型 */
    char endpoint[512]; /**< 上游基址 */
    char upstream_key_ref[1024]; /**< "env:NAME" | "pg:<blob>" | "" */
    char upstream_key[1024]; /**< resolved in-memory */
    int  weight; /**< weight > 0, default 1 */
    int  priority; /**< 0 = primary tier, 1 = fallback tier, etc. */
} upstream_target_t;

/** @brief Model route record (models row + resolved upstream key). */
typedef struct model_rec {
    char name[128]; /**< 模型名 */
    char provider[32]; /**< primary / fallback default */
    char endpoint[512]; /**< primary / fallback default */
    char upstream_key_ref[1024]; /**< "env:NAME" | "pg:<blob>" | "" */
    char default_params_json[1024]; /**< jansson object; default_params win < request */
    int  enabled; /**< 启用开关（1 启用） */
    char upstream_key[1024]; /**< filled by model_router, not stored */

    /* Multi-target additions */
    int               n_targets; /**< 目标数 */
    upstream_target_t targets[MAX_TARGETS_PER_MODEL]; /**< 多目标数组 */
    char lb_policy[32]; /**< "priority", "round_robin", "weighted", "weighted_round_robin" */
    char pricing_json[1024]; /**< jansson object with in_mtok, out_mtok, cached_mtok_discount */
} model_rec_t;

/** @brief One usage_daily row. */
typedef struct usage_row {
    long   key_id; /**< API key 主键 */
    char   model_name[128]; /**< 模型名 */
    time_t day; /**< midnight UTC */
    long   requests; /**< 当日累计请求数 */
    long   prompt_tokens; /**< 当日累计 prompt token */
    long   completion_tokens; /**< 当日累计 completion token */
    long   errors; /**< 当日累计错误数 */
    long   cached_prompt_tokens; /**< 缓存命中 prompt token */
} usage_row_t;

/** @brief One usage_requests (per-request audit) row. */
typedef struct usage_request_row {
    long     key_id; /**< API key 主键 */
    char     model_name[128]; /**< 模型名 */
    char     provider[32]; /**< 供应商类型 */
    int      http_status; /**< 上游 HTTP 状态码 */
    long     prompt_tokens; /**< prompt token 数 */
    long     completion_tokens; /**< completion token 数 */
    long     cached_prompt_tokens; /**< 缓存命中 prompt token */
    long     reasoning_tokens; /**< 推理 token 数 */
    uint64_t latency_ns; /**< 端到端延迟纳秒 */
    time_t   ts; /**< 请求时间戳 */
    char     guardrail_action[16]; /**< 护栏处置结果 */
} usage_request_row_t;

/** @brief Group record (groups row + key count). */
typedef struct group_rec {
    long   id; /**< 主键 */
    char   name[128]; /**< 组名 */
    long   key_count; /**< 组内 key 数 */
    time_t created_at; /**< 创建时间戳 */
    double monthly_budget_usd; /**< 0.0 = unlimited */
} group_rec_t;

/** @brief Guardrail rule record (guardrails_rules row). */
typedef struct guardrail_rule {
    long   id; /**< 主键 */
    char   rule_type[32]; /**< "keyword" | "regex" | "pii" */
    char   pattern[512]; /**< 匹配模式（关键词/正则/待脱敏文本特征） */
    char   action[32]; /**< "block" | "mask" */
    char   category[64]; /**< "general" | "profanity" | "safety" etc. */
    int    enabled; /**< 1 = true, 0 = false */
    time_t created_at; /**< 创建时间戳 */
} guardrail_rule_t;

/** @brief One cost attribution row. */
typedef struct cost_row {
    long   group_id; /**< 分组 id */
    char   model[128]; /**< 模型名 */
    long   prompt; /**< prompt token */
    long   completion; /**< completion token */
    long   cached; /**< 缓存命中 token */
    long   requests; /**< 请求数 */
    time_t bucket_day; /**< midnight UTC timestamp */
} cost_row_t;

/* update_key / update_model / update_provider field masks (bit flags). */
/** @brief key 更新掩码：限速字段。 */
#define KMASK_RATE (1 << 0)
/** @brief key 更新掩码：配额字段。 */
#define KMASK_QUOTA (1 << 1)
/** @brief key 更新掩码：模型白名单。 */
#define KMASK_ALLOWLIST (1 << 2)
/** @brief key 更新掩码：过期时间。 */
#define KMASK_EXPIRY (1 << 3)
/** @brief key 更新掩码：所属分组。 */
#define KMASK_GROUP (1 << 4)
/** @brief key 更新掩码：护栏开关。 */
#define KMASK_GUARDRAILS (1 << 5)
/** @brief key 更新掩码：月费用预算。 */
#define KMASK_MONTHLY_COST_BUDGET (1 << 6)
/** @brief key 更新掩码：月 token 预算。 */
#define KMASK_MONTHLY_TOKEN_BUDGET (1 << 7)

/** @brief model 更新掩码：端点。 */
#define MMASK_ENDPOINT (1 << 0)
/** @brief model 更新掩码：默认参数。 */
#define MMASK_PARAMS (1 << 1)
/** @brief model 更新掩码：启用开关。 */
#define MMASK_ENABLED (1 << 2)
/** @brief model 更新掩码：上游密钥引用。 */
#define MMASK_KEYREF (1 << 3)
/** @brief model 更新掩码：多目标列表。 */
#define MMASK_TARGETS (1 << 4)
/** @brief model 更新掩码：负载策略。 */
#define MMASK_LB_POLICY (1 << 5)
/** @brief model 更新掩码：定价。 */
#define MMASK_PRICING (1 << 6)

/** @brief provider 更新掩码：供应商类型。 */
#define PMASK_TYPE (1 << 0)
/** @brief provider 更新掩码：端点。 */
#define PMASK_ENDPOINT (1 << 1)
/** @brief provider 更新掩码：API 密钥。 */
#define PMASK_API_KEY (1 << 2)
/** @brief provider 更新掩码：模型清单。 */
#define PMASK_MODELS (1 << 3)
/** @brief provider 更新掩码：启用开关。 */
#define PMASK_ENABLED (1 << 4)

/** @brief Provider record (providers row). */
typedef struct provider_rec {
    long   id; /**< 主键 */
    char   name[64]; /**< 供应商名 */
    char   provider_type[32]; /**< 供应商类型 */
    char   endpoint[512]; /**< 上游基址 */
    char   api_key[1024]; /**< 上游密钥 */
    char** models; /**< 模型清单（堆数组） */
    int    n_models; /**< 模型数 */
    int    enabled; /**< 启用开关（1 启用） */
    time_t created_at; /**< 创建时间戳 */
} provider_rec_t;

/** @brief Uniform persistence operations; real libpq or in-memory fakes.
 *
 *  All functions return 0 on success, -1 on error. out parameters may be
 *  NULL when unused. @p ctx is the implementation's private state.
 *  @c get_key_by_hash is the exception: 0 = found, 1 = key definitively
 *  absent, -1 = storage error (callers must not treat -1 as "missing").
 *  Thread-safety: implementations MUST be re-entrant safe; the libpq
 *  implementation serializes with an internal mutex. */
typedef struct pg_ops {
    void* ctx; /**< 实现私有状态 */

    int (*get_key_by_hash)(void* ctx, const char* key_hash, key_rec_t* out); /**< 按哈希查 key：0 命中，1 确无，-1 存储错误。 */
    int (*list_keys)(void* ctx, key_rec_t* out, int cap, int* n); /**< 列出 key（out 容量 cap，n 回条数）。 */
    int (*get_key_by_id)(void* ctx, long key_id, key_rec_t* out); /**< 按数字 id 查 key。 */
    int (*list_models)(void* ctx, model_rec_t* out, int cap, int* n); /**< 列出模型路由。 */
    int (*get_model)(void* ctx, const char* name, model_rec_t* out); /**< 按模型名查路由。 */

    int (*create_key)(void* ctx, const key_rec_t* k, long* out_key_id); /**< 新建 key，out_key_id 回主键。 */
    int (*update_key)(void* ctx, const key_rec_t* k, int mask); /**< 按 KMASK_* 掩码更新 key 字段。 */
    int (*revoke_key)(void* ctx, long key_id); /**< 吊销 key。 */

    int (*create_model)(void* ctx, const model_rec_t* m); /**< 新建模型路由。 */
    int (*update_model)(void* ctx, const model_rec_t* m, int mask); /**< 按 MMASK_* 掩码更新模型路由。 */
    int (*delete_model)(void* ctx, const char* name); /**< 按模型名删除路由。 */

    int (*list_providers)(void* ctx, provider_rec_t* out, int cap, int* n); /**< 列出上游供应商。 */
    int (*get_provider)(void* ctx, long id, provider_rec_t* out); /**< 按数字 id 查供应商。 */
    int (*create_provider)(void* ctx, const provider_rec_t* p, long* out_id); /**< 新建供应商，out_id 回主键。 */
    int (*update_provider)(void* ctx, const provider_rec_t* p, int mask); /**< 按 PMASK_* 掩码更新供应商。 */
    int (*delete_provider)(void* ctx, long id); /**< 按数字 id 删除供应商。 */

    int (*flush_usage)(void* ctx, const usage_row_t* rows, int n); /**< 批量落库日用量行。 */
    int (*query_usage)(void*        ctx,
                       long         key_id,
                       const char*  model,
                       time_t       from,
                       time_t       to,
                       usage_row_t* out,
                       int          cap,
                        int*         n); /**< 按 key/模型/时间范围查日用量。 */
    int (*flush_usage_requests)(void* ctx, const usage_request_row_t* rows, int n); /**< 批量落库逐请求审计行。 */
    int (*query_usage_requests)(
        void* ctx, long key_id, time_t since, usage_request_row_t* out, int cap, int* n); /**< 按 key/起始时间查逐请求审计行。 */

    int (*create_group)(void* ctx, const char* name, long* out_id); /**< 新建分组，out_id 回主键。 */
    int (*list_groups)(void* ctx, group_rec_t* out, int cap, int* n); /**< 列出分组。 */
    int (*patch_group)(void* ctx, long id, const char* name); /**< 按数字 id 改组名。 */
    int (*patch_group_budget)(void* ctx, long id, double budget); /**< 按数字 id 改分组月预算。 */
    int (*delete_group)(void* ctx, long id); /**< 按数字 id 删除分组。 */
    int (*count_keys_in_group)(void* ctx, long group_id, long* n); /**< 统计分组内 key 数。 */
    int (*query_cost)(void* ctx, long since_s, long until_s, cost_row_t* out, int cap, int* n); /**< 按时间范围查费用归因行。 */

    int (*list_guardrails_rules)(void* ctx, guardrail_rule_t* out, int cap, int* n); /**< 列出护栏规则。 */
    int (*create_guardrails_rule)(void* ctx, const guardrail_rule_t* rule, long* out_id); /**< 新建护栏规则，out_id 回主键。 */
    int (*update_guardrails_rule)(void* ctx, const guardrail_rule_t* rule); /**< 按 id 全字段更新护栏规则。 */
    int (*delete_guardrails_rule)(void* ctx, long id); /**< 按数字 id 删除护栏规则。 */
} pg_ops_t;

/** @brief 存储句柄（opaque；libpq 连接或 fake 上下文持有者）。 */
typedef struct pg_store pg_store_t;

/** @brief Open a store over @p dsn.
 * @param dsn  PostgreSQL connection string; ignored when @p ops is non-NULL.
 * @param ops  custom ops table (unit-test fakes); when NULL the real libpq
 *             ops are used and @p ctx is set to the internal connection.
 * @return store handle, or NULL on connection failure.
 * @note Ownership: when @p ops is provided, its ->ctx is used as-is; when
 *       NULL, the store allocates the libpq context and frees it on close. */
pg_store_t* pg_store_open(const char* dsn, const pg_ops_t* ops);

/** @brief Close the store; when it owns a libpq connection, disconnects it. */
void pg_store_close(pg_store_t* ps);

/** @brief Apply embedded schema.sql for any unapplied version. @return 0 ok, -1 error. */
int pg_store_migrate(pg_store_t* ps);

/** @brief Access the live ops table (real or fake). */
const pg_ops_t* pg_store_ops(const pg_store_t* ps);

/** @brief Free a key_rec_t populated by get_key_by_hash (frees allowed_models). */
void key_rec_free(key_rec_t* k);

/** @brief Free a model_rec_t populated by list_models (frees endpoint copies). */
void model_rec_free(model_rec_t* m);

/** @brief Free a provider_rec_t populated by list_providers or get_provider. */
void provider_rec_free(provider_rec_t* p);

/** @brief 列出 guardrails 规则。
 *  @param ps   存储句柄。
 *  @param out  调用方提供的数组，容量 ≥ @p cap。
 *  @param cap  @p out 最大容纳条数。
 *  @param n    接收实际写入条数。
 *  @return 0 成功；-1 存储错误。 */
int pg_store_list_guardrails_rules(const pg_store_t* ps, guardrail_rule_t* out, int cap, int* n);
/** @brief 新建 guardrails 规则。
 *  @param ps 存储句柄。
 *  @param rule 规则内容（id 字段忽略）。
 *  @param out_id 接收新规则 id。
 *  @return 0 成功；-1 存储错误。 */
int pg_store_create_guardrails_rule(const pg_store_t* ps, const guardrail_rule_t* rule, long* out_id);
/** @brief 按 id 全字段更新 guardrails 规则。
 *  @return 0 成功；-1 存储错误。 */
int pg_store_update_guardrails_rule(const pg_store_t* ps, const guardrail_rule_t* rule);
/** @brief 按 id 删除 guardrails 规则。
 *  @return 0 成功；-1 存储错误。 */
int pg_store_delete_guardrails_rule(const pg_store_t* ps, long id);

#endif /* AIGATE_PG_STORE_H */
