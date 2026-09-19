/** @file pg_store.h
 *  @brief PostgreSQL persistence layer: schema migration + an ops table so
 *  that every caller (auth cache, admin API, flush worker) talks to a
 *  uniform interface that unit tests can fake (spec section 3).
 *
 *  Canonical record types are defined here and shared by all modules.
 */
#ifndef AIGATE_PG_STORE_H
#define AIGATE_PG_STORE_H

#include <time.h>

/** @brief Client API key record (api_keys row; allowed_models is a copy). */
typedef struct key_rec {
    long   key_id;
    char   key_hash[65];      /* 64 lowercase hex + NUL */
    char   name[128];
    char** allowed_models;    /* NUL-terminated-ish: exactly n_allowed entries */
    int    n_allowed;         /* 0 = all models allowed */
    int    rate_qps;          /* 0 = unlimited */
    long   daily_token_quota; /* 0 = unlimited */
    time_t expires_at;
    int    has_expiry;
    int    revoked;
} key_rec_t;

#define MAX_TARGETS_PER_MODEL 8

typedef struct upstream_target {
    char provider[32];
    char endpoint[512];
    char upstream_key_ref[1024];
    char upstream_key[1024]; /* resolved in-memory */
    int  weight;             /* weight > 0, default 1 */
    int  priority;           /* 0 = primary tier, 1 = fallback tier, etc. */
} upstream_target_t;

/** @brief Model route record (models row + resolved upstream key). */
typedef struct model_rec {
    char name[128];
    char provider[32];              /* primary / fallback default */
    char endpoint[512];             /* primary / fallback default */
    char upstream_key_ref[1024];    /* "env:NAME" | "pg:<blob>" | "" */
    char default_params_json[1024]; /* jansson object; default_params win < request */
    int  enabled;
    char upstream_key[1024];        /* filled by model_router, not stored */

    /* Multi-target additions */
    int               n_targets;
    upstream_target_t targets[MAX_TARGETS_PER_MODEL];
    char              lb_policy[32]; /* "priority", "round_robin", "weighted", "weighted_round_robin" */
} model_rec_t;

/** @brief One usage_daily row. */
typedef struct usage_row {
    long   key_id;
    char   model_name[128];
    time_t day; /* midnight UTC */
    long   requests, prompt_tokens, completion_tokens, errors;
    long   cached_prompt_tokens;
} usage_row_t;

/* update_key / update_model / update_provider field masks (bit flags). */
#define KMASK_RATE (1 << 0)
#define KMASK_QUOTA (1 << 1)
#define KMASK_ALLOWLIST (1 << 2)
#define KMASK_EXPIRY (1 << 3)

#define MMASK_ENDPOINT (1 << 0)
#define MMASK_PARAMS (1 << 1)
#define MMASK_ENABLED (1 << 2)
#define MMASK_KEYREF (1 << 3)
#define MMASK_TARGETS (1 << 4)
#define MMASK_LB_POLICY (1 << 5)

#define PMASK_TYPE (1 << 0)
#define PMASK_ENDPOINT (1 << 1)
#define PMASK_API_KEY (1 << 2)
#define PMASK_MODELS (1 << 3)
#define PMASK_ENABLED (1 << 4)

/** @brief Provider record (providers row). */
typedef struct provider_rec {
    long   id;
    char   name[64];
    char   provider_type[32];
    char   endpoint[512];
    char   api_key[1024];
    char** models;
    int    n_models;
    int    enabled;
    time_t created_at;
} provider_rec_t;

/** @brief Uniform persistence operations; real libpq or in-memory fakes.
 *
 *  All functions return 0 on success, -1 on error. out parameters may be
 *  NULL when unused. @p ctx is the implementation's private state.
 *  Thread-safety: implementations MUST be re-entrant safe; the libpq
 *  implementation serializes with an internal mutex. */
typedef struct pg_ops {
    void* ctx;

    int (*get_key_by_hash)(void* ctx, const char* key_hash, key_rec_t* out);
    int (*list_keys)(void* ctx, key_rec_t* out, int cap, int* n);
    int (*get_key_by_id)(void* ctx, long key_id, key_rec_t* out);
    int (*list_models)(void* ctx, model_rec_t* out, int cap, int* n);
    int (*get_model)(void* ctx, const char* name, model_rec_t* out);

    int (*create_key)(void* ctx, const key_rec_t* k, long* out_key_id);
    int (*update_key)(void* ctx, const key_rec_t* k, int mask);
    int (*revoke_key)(void* ctx, long key_id);

    int (*create_model)(void* ctx, const model_rec_t* m);
    int (*update_model)(void* ctx, const model_rec_t* m, int mask);
    int (*delete_model)(void* ctx, const char* name);

    int (*list_providers)(void* ctx, provider_rec_t* out, int cap, int* n);
    int (*get_provider)(void* ctx, long id, provider_rec_t* out);
    int (*create_provider)(void* ctx, const provider_rec_t* p, long* out_id);
    int (*update_provider)(void* ctx, const provider_rec_t* p, int mask);
    int (*delete_provider)(void* ctx, long id);

    int (*flush_usage)(void* ctx, const usage_row_t* rows, int n);
    int (*query_usage)(void*        ctx,
                       long         key_id,
                       const char*  model,
                       time_t       from,
                       time_t       to,
                       usage_row_t* out,
                       int          cap,
                       int*         n);
} pg_ops_t;

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

#endif /* AIGATE_PG_STORE_H */
