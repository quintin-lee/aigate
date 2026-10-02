/** @file pg_store.h
 *  @brief PostgreSQL persistence layer: schema migration + an ops table so
 *  that every caller (auth cache, admin API, flush worker) talks to a
 *  uniform interface that unit tests can fake (spec section 3).
 *
 *  Canonical record types are defined here and shared by all modules.
 */

/**
 * @defgroup group_store Storage layer
 * @brief Storage: Postgres, Redis, and usage metering.
 */
#ifndef AIGATE_PG_STORE_H
#define AIGATE_PG_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "policy/shadow.h"
#include "policy/prompt_compressor.h"
#include "policy/cache_optimizer.h"

/** @brief Client API key record (api_keys row; allowed_models is a copy). */
typedef struct key_rec {
    long   key_id;               /**< Primary key */
    char   key_hash[65];         /**< 64 lowercase hex + NUL */
    char   name[128];            /**< Name */
    char** allowed_models;       /**< NUL-terminated-ish: exactly n_allowed entries */
    int    n_allowed;            /**< 0 = all models allowed */
    int    rate_qps;             /**< 0 = unlimited */
    long   daily_token_quota;    /**< 0 = unlimited */
    time_t expires_at;           /**< Expiration timestamp */
    int    has_expiry;           /**< Whether expiry is set (1 takes effect) */
    int    revoked;              /**< Revocation flag (1 means revoked) */
    long   group_id;             /**< 0 = ungrouped */
    int    guardrails_enabled;   /**< 1 = enabled (default), 0 = disabled */
    double monthly_cost_budget;  /**< 0.0 = unlimited */
    long   monthly_token_budget; /**< 0 = unlimited */
    char   system_prompt[4096];  /**< Optional prompt template */
    int    prompt_mode;          /**< 0=prepend, 1=append, 2=override */
} key_rec_t;

/** @brief Per-model multi-target cap. */
#define MAX_TARGETS_PER_MODEL 8

/** @brief Single upstream target: provider, base URL, key reference, and load weight. */
typedef struct upstream_target {
    char provider[32];           /**< Provider type */
    char endpoint[512];          /**< Upstream base URL */
    char upstream_key_ref[1024]; /**< "env:NAME" | "pg:<blob>" | "" */
    char upstream_key[1024];     /**< resolved in-memory */
    int  weight;                 /**< weight > 0, default 1 */
    int  priority;               /**< 0 = primary tier, 1 = fallback tier, etc. */
} upstream_target_t;

/** @brief Model route record (models row + resolved upstream key). */
typedef struct model_rec {
    char name[128];                 /**< Model name */
    char provider[32];              /**< primary / fallback default */
    char endpoint[512];             /**< primary / fallback default */
    char upstream_key_ref[1024];    /**< "env:NAME" | "pg:<blob>" | "" */
    char default_params_json[1024]; /**< jansson object; default_params win < request */
    int  enabled;                   /**< Enabled switch (1 means enabled) */
    char upstream_key[1024];        /**< filled by model_router, not stored */

    /* Multi-target additions */
    int               n_targets;                      /**< Target count */
    upstream_target_t targets[MAX_TARGETS_PER_MODEL]; /**< Multi-target array */
    char lb_policy[32];       /**< "priority", "round_robin", "weighted", "weighted_round_robin" */
    char pricing_json[1024];  /**< jansson object with in_mtok, out_mtok, cached_mtok_discount */
    char system_prompt[4096]; /**< Optional prompt template */
    int  prompt_mode;         /**< 0=prepend, 1=append, 2=override */
    int  hedged_delay_ms;     /**< 0 for auto-P95, >0 for static ms delay */
    int  hedge_budget_pct;    /**< Max % of requests that can trigger hedge (default: 15) */
    bool hedged_enabled;      /**< True if hedged speculative execution is active */
} model_rec_t;

/** @brief One usage_daily row. */
typedef struct usage_row {
    long   key_id;               /**< API key primary key */
    char   model_name[128];      /**< Model name */
    time_t day;                  /**< midnight UTC */
    long   requests;             /**< Daily accumulated request count */
    long   prompt_tokens;        /**< Daily accumulated prompt tokens */
    long   completion_tokens;    /**< Daily accumulated completion tokens */
    long   errors;               /**< Daily accumulated error count */
    long   cached_prompt_tokens; /**< Cache-hit prompt tokens */
} usage_row_t;

/** @brief One usage_requests (per-request audit) row. */
typedef struct usage_request_row {
    long     key_id;               /**< API key primary key */
    char     model_name[128];      /**< Model name */
    char     provider[32];         /**< Provider type */
    int      http_status;          /**< Upstream HTTP status code */
    long     prompt_tokens;        /**< Prompt token count */
    long     completion_tokens;    /**< Completion token count */
    long     cached_prompt_tokens; /**< Cache-hit prompt tokens */
    long     reasoning_tokens;     /**< Reasoning token count */
    uint64_t latency_ns;           /**< End-to-end latency in nanoseconds */
    time_t   ts;                   /**< Request timestamp */
    char     guardrail_action[16]; /**< Guardrails action taken */
} usage_request_row_t;

/** @brief Group record (groups row + key count). */
typedef struct group_rec {
    long   id;                 /**< Primary key */
    char   name[128];          /**< Group name */
    long   key_count;          /**< Key count in the group */
    time_t created_at;         /**< Creation timestamp */
    double monthly_budget_usd; /**< 0.0 = unlimited */
} group_rec_t;

/** @brief Guardrail rule record (guardrails_rules row). */
typedef struct guardrail_rule {
    long   id;                  /**< Primary key */
    char   rule_type[32];       /**< "keyword" | "regex" | "pii" | "webhook" */
    char   pattern[512];        /**< Match pattern (keyword/regex/webhook URL) */
    char   action[32];          /**< "block" | "mask" */
    char   category[64];        /**< "general" | "profanity" | "safety" etc. */
    int    enabled;             /**< 1 = true, 0 = false */
    char   webhook_secret[256]; /**< Optional Bearer token / auth secret */
    int    timeout_ms;          /**< Webhook HTTP timeout in ms (default: 500) */
    char   fail_mode[16];       /**< "open" (fail-open) | "closed" (fail-closed) */
    char   phase[16];           /**< "inbound" | "outbound" | "both" */
    time_t created_at;          /**< Creation timestamp */
} guardrail_rule_t;

/** @brief One cost attribution row. */
typedef struct cost_row {
    long   group_id;   /**< Group id */
    char   model[128]; /**< Model name */
    long   prompt;     /**< prompt token */
    long   completion; /**< completion token */
    long   cached;     /**< Cache-hit tokens */
    long   requests;   /**< Request count */
    time_t bucket_day; /**< midnight UTC timestamp */
} cost_row_t;

/* update_key / update_model / update_provider field masks (bit flags). */
/** @brief Key update mask: rate-limit fields. */
#define KMASK_RATE (1 << 0)
/** @brief Key update mask: quota fields. */
#define KMASK_QUOTA (1 << 1)
/** @brief Key update mask: model allowlist. */
#define KMASK_ALLOWLIST (1 << 2)
/** @brief Key update mask: expiry time. */
#define KMASK_EXPIRY (1 << 3)
/** @brief Key update mask: owning group. */
#define KMASK_GROUP (1 << 4)
/** @brief Key update mask: guardrails switch. */
#define KMASK_GUARDRAILS (1 << 5)
/** @brief Key update mask: monthly cost budget. */
#define KMASK_MONTHLY_COST_BUDGET (1 << 6)
/** @brief Key update mask: monthly token budget. */
#define KMASK_MONTHLY_TOKEN_BUDGET (1 << 7)
/** @brief Key update mask: system prompt template. */
#define KMASK_SYSTEM_PROMPT (1 << 8)
/** @brief Key update mask: prompt template mode. */
#define KMASK_PROMPT_MODE (1 << 9)

/** @brief Model update mask: endpoint. */
#define MMASK_ENDPOINT (1 << 0)
/** @brief Model update mask: default parameters. */
#define MMASK_PARAMS (1 << 1)
/** @brief Model update mask: enabled switch. */
#define MMASK_ENABLED (1 << 2)
/** @brief Model update mask: upstream key reference. */
#define MMASK_KEYREF (1 << 3)
/** @brief Model update mask: multi-target list. */
#define MMASK_TARGETS (1 << 4)
/** @brief Model update mask: load-balancing policy. */
#define MMASK_LB_POLICY (1 << 5)
/** @brief Model update mask: pricing. */
#define MMASK_PRICING (1 << 6)
/** @brief Model update mask: system prompt template. */
#define MMASK_SYSTEM_PROMPT (1 << 7)
/** @brief Model update mask: prompt template mode. */
#define MMASK_PROMPT_MODE (1 << 8)
/** @brief Model update mask: hedged delay in ms. */
#define MMASK_HEDGED_DELAY (1 << 9)
/** @brief Model update mask: hedge budget percentage. */
#define MMASK_HEDGE_BUDGET (1 << 10)
/** @brief Model update mask: hedged enabled boolean. */
#define MMASK_HEDGED_ENABLED (1 << 11)

/** @brief Provider update mask: provider type. */
#define PMASK_TYPE (1 << 0)
/** @brief Provider update mask: endpoint. */
#define PMASK_ENDPOINT (1 << 1)
/** @brief Provider update mask: API key. */
#define PMASK_API_KEY (1 << 2)
/** @brief Provider update mask: model list. */
#define PMASK_MODELS (1 << 3)
/** @brief Provider update mask: enabled switch. */
#define PMASK_ENABLED (1 << 4)

/** @brief Provider record (providers row). */
typedef struct provider_rec {
    long   id;                /**< Primary key */
    char   name[64];          /**< Provider name */
    char   provider_type[32]; /**< Provider type */
    char   endpoint[512];     /**< Upstream base URL */
    char   api_key[1024];     /**< Upstream key */
    char** models;            /**< Model list (heap array) */
    int    n_models;          /**< Model count */
    int    enabled;           /**< Enabled switch (1 means enabled) */
    time_t created_at;        /**< Creation timestamp */
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
    void* ctx; /**< Implementation-private state */

    int (*get_key_by_hash)(
        void*       ctx,
        const char* key_hash,
        key_rec_t*  out); /**< Look up a key by hash: 0 hit, 1 definite miss, -1 storage error. */
    int (*list_keys)(void*      ctx,
                     key_rec_t* out,
                     int        cap,
                     int*       n);       /**< List keys (out capacity cap, n returns the count). */
    int (*get_key_by_id)(void*      ctx,
                         long       key_id,
                         key_rec_t* out); /**< Look up a key by numeric id. */
    int (*list_models)(void* ctx, model_rec_t* out, int cap, int* n); /**< List model routes. */
    int (*get_model)(void*        ctx,
                     const char*  name,
                     model_rec_t* out); /**< Look up a route by model name. */

    int (*create_key)(void*            ctx,
                      const key_rec_t* k,
                      long* out_key_id); /**< Create a key, out_key_id returns the primary key. */
    int (*update_key)(void*            ctx,
                      const key_rec_t* k,
                      int              mask);             /**< Update key fields by KMASK_* mask. */
    int (*revoke_key)(void* ctx, long key_id);            /**< Revoke a key. */

    int (*create_model)(void* ctx, const model_rec_t* m); /**< Create a model route. */
    int (*update_model)(void*              ctx,
                        const model_rec_t* m,
                        int                mask);     /**< Update a model route by MMASK_* mask. */
    int (*delete_model)(void* ctx, const char* name); /**< Delete a route by model name. */

    int (*list_providers)(void*           ctx,
                          provider_rec_t* out,
                          int             cap,
                          int*            n); /**< List upstream providers. */
    int (*get_provider)(void*           ctx,
                        long            id,
                        provider_rec_t* out); /**< Look up a provider by numeric id. */
    int (*create_provider)(void*                 ctx,
                           const provider_rec_t* p,
                           long* out_id); /**< Create a provider, out_id returns the primary key. */
    int (*update_provider)(void*                 ctx,
                           const provider_rec_t* p,
                           int                   mask); /**< Update a provider by PMASK_* mask. */
    int (*delete_provider)(void* ctx, long id);         /**< Delete a provider by numeric id. */

    int (*flush_usage)(void*              ctx,
                       const usage_row_t* rows,
                       int                n); /**< Batch-persist daily-usage rows. */
    int (*query_usage)(void*        ctx,
                       long         key_id,
                       const char*  model,
                       time_t       from,
                       time_t       to,
                       usage_row_t* out,
                       int          cap,
                       int*         n); /**< Query daily usage by key/model/time range. */
    int (*flush_usage_requests)(void*                      ctx,
                                const usage_request_row_t* rows,
                                int n); /**< Batch-persist per-request audit rows. */
    int (*query_usage_requests)(void*                ctx,
                                long                 key_id,
                                time_t               since,
                                usage_request_row_t* out,
                                int                  cap,
                                int* n); /**< Query per-request audit rows by key/start time. */

    int (*create_group)(void*       ctx,
                        const char* name,
                        long*       out_id); /**< Create a group, out_id returns the primary key. */
    int (*list_groups)(void* ctx, group_rec_t* out, int cap, int* n); /**< List groups. */
    int (*patch_group)(void* ctx, long id, const char* name); /**< Rename a group by numeric id. */
    int (*patch_group_budget)(void*  ctx,
                              long   id,
                              double budget); /**< Change a group's monthly budget by numeric id. */
    int (*delete_group)(void* ctx, long id);  /**< Delete a group by numeric id. */
    int (*count_keys_in_group)(void* ctx, long group_id, long* n); /**< Count keys in a group. */
    int (*query_cost)(void*       ctx,
                      long        since_s,
                      long        until_s,
                      cost_row_t* out,
                      int         cap,
                      int*        n); /**< Query cost-attribution rows by time range. */

    int (*list_guardrails_rules)(void*             ctx,
                                 guardrail_rule_t* out,
                                 int               cap,
                                 int*              n); /**< List guardrails rules. */
    int (*create_guardrails_rule)(
        void*                   ctx,
        const guardrail_rule_t* rule,
        long* out_id); /**< Create a guardrails rule, out_id returns the primary key. */
    int (*update_guardrails_rule)(
        void*                   ctx,
        const guardrail_rule_t* rule);       /**< Full-field update of a guardrails rule by id. */
    int (*delete_guardrails_rule)(void* ctx,
                                  long  id); /**< Delete a guardrails rule by numeric id. */

    int (*list_shadow_rules)(void*          ctx,
                             shadow_rule_t* out,
                             int            cap,
                             int*           n); /**< List traffic shadow/canary rules. */
    int (*create_shadow_rule)(void*                ctx,
                              const shadow_rule_t* rule,
                              long* out_id); /**< Create shadow rule, out_id returns primary key. */
    int (*update_shadow_rule)(
        void* ctx, const shadow_rule_t* rule);     /**< Full-field update of shadow rule by id. */
    int (*delete_shadow_rule)(void* ctx, long id); /**< Delete shadow rule by numeric id. */

    int (*list_compressor_rules)(void*              ctx,
                                 compressor_rule_t* out,
                                 int                cap,
                                 int*               n); /**< List prompt compressor rules. */
    int (*upsert_compressor_rule)(
        void* ctx, const compressor_rule_t* rule); /**< Upsert prompt compressor rule by UUID id. */
    int (*delete_compressor_rule)(void*       ctx,
                                  const char* id); /**< Delete prompt compressor rule by UUID id. */

    int (*list_cache_optimizer_rules)(void*                   ctx,
                                      cache_optimizer_rule_t* out,
                                      int                     cap,
                                      int* n); /**< List prompt cache optimizer rules. */
    int (*upsert_cache_optimizer_rule)(
        void*                         ctx,
        const cache_optimizer_rule_t* rule); /**< Upsert prompt cache optimizer rule by UUID id. */
    int (*delete_cache_optimizer_rule)(
        void* ctx, const char* id);          /**< Delete prompt cache optimizer rule by UUID id. */
} pg_ops_t;

/** @brief Storage handle (opaque; holder of a libpq connection or a fake context). */
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

/** @brief List guardrails rules.
 *  @param ps   Storage handle.
 *  @param out  Caller-provided array with capacity >= @p cap.
 *  @param cap  Maximum entries @p out can hold.
 *  @param n    Receives the actual written count.
 *  @return 0 on success; -1 on storage error. */
int pg_store_list_guardrails_rules(const pg_store_t* ps, guardrail_rule_t* out, int cap, int* n);
/** @brief Create a guardrails rule.
 *  @param ps Storage handle.
 *  @param rule Rule content (the id field is ignored).
 *  @param out_id Receives the new rule id.
 *  @return 0 on success; -1 on storage error. */
int
pg_store_create_guardrails_rule(const pg_store_t* ps, const guardrail_rule_t* rule, long* out_id);
/** @brief Full-field update of a guardrails rule by id.
 *  @return 0 on success; -1 on storage error. */
int pg_store_update_guardrails_rule(const pg_store_t* ps, const guardrail_rule_t* rule);
/** @brief Delete a guardrails rule by id.
 *  @return 0 on success; -1 on storage error. */
int pg_store_delete_guardrails_rule(const pg_store_t* ps, long id);

/** @brief List traffic shadow/canary rules.
 *  @param ps   Storage handle.
 *  @param out  Output array.
 *  @param cap  Maximum entries.
 *  @param n    Receives written count.
 *  @return 0 on success, -1 on storage error. */
int pg_store_list_shadow_rules(const pg_store_t* ps, shadow_rule_t* out, int cap, int* n);

/** @brief Create traffic shadow/canary rule.
 *  @param ps     Storage handle.
 *  @param rule   Rule content.
 *  @param out_id Receives new rule ID.
 *  @return 0 on success, -1 on storage error. */
int pg_store_create_shadow_rule(const pg_store_t* ps, const shadow_rule_t* rule, long* out_id);

/** @brief Update traffic shadow/canary rule.
 *  @param ps   Storage handle.
 *  @param rule Rule content.
 *  @return 0 on success, -1 on storage error. */
int pg_store_update_shadow_rule(const pg_store_t* ps, const shadow_rule_t* rule);

/** @brief Delete traffic shadow/canary rule.
 *  @param ps Storage handle.
 *  @param id Rule ID.
 *  @return 0 on success, -1 on storage error. */
int pg_store_delete_shadow_rule(const pg_store_t* ps, long id);

/** @brief List prompt compressor rules.
 *  @param ps   Storage handle.
 *  @param out  Output array.
 *  @param cap  Maximum entries.
 *  @param n    Receives written count.
 *  @return 0 on success, -1 on storage error. */
int pg_store_list_compressor_rules(const pg_store_t* ps, compressor_rule_t* out, int cap, int* n);

/** @brief Create or update prompt compressor rule.
 *  @param ps   Storage handle.
 *  @param rule Rule content.
 *  @return 0 on success, -1 on storage error. */
int pg_store_upsert_compressor_rule(const pg_store_t* ps, const compressor_rule_t* rule);

/** @brief Delete prompt compressor rule.
 *  @param ps Storage handle.
 *  @param id Rule UUID.
 *  @return 0 on success, -1 on storage error. */
int pg_store_delete_compressor_rule(const pg_store_t* ps, const char* id);

/** @brief List prompt cache optimizer rules.
 *  @param ps   Storage handle.
 *  @param out  Output array.
 *  @param cap  Maximum entries.
 *  @param n    Receives written count.
 *  @return 0 on success, -1 on storage error. */
int pg_store_list_cache_optimizer_rules(const pg_store_t*       ps,
                                        cache_optimizer_rule_t* out,
                                        int                     cap,
                                        int*                    n);

/** @brief Create or update prompt cache optimizer rule.
 *  @param ps   Storage handle.
 *  @param rule Rule content.
 *  @return 0 on success, -1 on storage error. */
int pg_store_upsert_cache_optimizer_rule(const pg_store_t* ps, const cache_optimizer_rule_t* rule);

/** @brief Delete prompt cache optimizer rule.
 *  @param ps Storage handle.
 *  @param id Rule UUID.
 *  @return 0 on success, -1 on storage error. */
int pg_store_delete_cache_optimizer_rule(const pg_store_t* ps, const char* id);

#endif /* AIGATE_PG_STORE_H */
