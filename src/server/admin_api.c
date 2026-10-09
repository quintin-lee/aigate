/** @file admin_api.c
 *  @brief /admin/v1 management plane (see admin_api.h).
 *
 *  All endpoints are Bearer-protected: the raw admin token is SHA-256 hashed
 *  and constant-time compared against the configured hash. Writes go
 *  through the store ops and invalidate the pipeline caches (auth key LRU,
 *  model router LRU) so the hot path sees the change on the next request.
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "admin_api.h"
#include "aigate_log.h"
#include "auth_key.h"
#include "budget_enforce.h"
#include "circuit_breaker.h"
#include "guardrails.h"
#include "model_router.h"
#include "provider_adapter.h"
#include "redis_client.h"
#include "redis_pool.h"
#include "redis_scripts.h"
#include "secrets.h"
#include "sha256.h"
#include "upstream_client.h"
#include "health_prober.h"
#include "event_bus.h"
#include "response_cache.h"
#include "observe/tracer.h"
#include "observe/audit_logger.h"
#include "policy/shadow.h"
#include "policy/prompt_compressor.h"
#include "policy/cache_optimizer.h"

#include <jansson.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/rand.h>

/** @brief Max keys/models/providers/usage rows staged per list response. */
#define KEY_LIST_CAP 512
/** @copydoc KEY_LIST_CAP */
#define MODEL_LIST_CAP 256
/** @copydoc KEY_LIST_CAP */
#define PROVIDER_LIST_CAP 128
/** @copydoc KEY_LIST_CAP */
#define USAGE_LIST_CAP 256

/* ------------------------------------------------------------ helpers */

/** @brief Bounded copy: return early when cap is 0; store empty string for NULL src; truncate overlong input with NUL termination. */
static void
copy_field(char* dst, size_t cap, const char* src)
{
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int
admin_auth_ok(admin_ctx_t* adm, const char* bearer)
{
    if (bearer == NULL || bearer[0] == '\0') {
        return 0;
    }
    char digest[65];
    if (sha256_hex(bearer, strlen(bearer), digest) != 0) {
        return 0;
    }
    return sha256_hex_equal(digest, (const char*)adm->admin_token_hash) == 1;
}

/* ------------------------------------------------------------ brute-force lockout */

/** @brief Number of in-process admin lockout hash slots. */
#define LOCKOUT_SLOTS 128

/* Policy is process-wide; transport sets it from env at start-up.
 * Values outside [2..1000] / [5..3600] keep the current value (guards
 * against a misconfig of 0 fails = lock everyone out, or a window so
 * short the guard is useless). */
static int g_lockout_fails = 10;
static int g_lockout_window_s = 300;

/* Optional Redis pool for distributed lockout (NULL = in-process only). */
static redis_pool_t* g_lockout_pool = NULL;
/** Distributed lockout Lua script SHA (loaded with g_lockout_pool init; empty string means not loaded). */
static char g_lockout_sha[48] = {0};

/** @brief One in-process admin-lockout hash slot. */
typedef struct {
    char           ip[32];     /**< keyed client IP text */
    _Atomic long   fails;      /**< failures inside the current window */
    _Atomic time_t first_fail; /**< window start (seconds since epoch) */
    int            in_use;     /**< slot occupied flag */
} lockout_slot_t;

/** Local admin lockout counter slots (sharded by IP, guarded by g_lockout_mtx). */
static lockout_slot_t g_lockout[LOCKOUT_SLOTS];
/** Local lockout slot mutex. */
static pthread_mutex_t g_lockout_mtx = PTHREAD_MUTEX_INITIALIZER;

/** @brief FNV-1a hash of the IP string modulo slot count (local lockout table shard lookup). */
static unsigned
lockout_slot_for(const char* ip)
{
    /* FNV-1a */
    unsigned h = 2166136261u;
    for (const char* p = ip; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    return h % LOCKOUT_SLOTS;
}

/** @brief 1 when @p ip is inside an active lockout window. */
static int
lockout_hit(const char* ip)
{
    if (ip == NULL) {
        return 0;
    }

    if (g_lockout_pool != NULL) {
        char redis_key[80];
        snprintf(redis_key, sizeof(redis_key), "aigate:lockout:%s", ip);
        redisContext* c = redis_pool_acquire(g_lockout_pool);
        if (c == NULL) {
            /* Fail-Closed: Redis down → treat as locked out to prevent bypass */
            return 1;
        }
        redisReply* reply = (redisReply*)redisCommand(c, "GET %s", redis_key);
        int         hit = 0;
        if (reply != NULL && reply->type == REDIS_REPLY_STRING) {
            long fails = strtol(reply->str, NULL, 10);
            hit = (fails >= g_lockout_fails) ? 1 : 0;
        } else if (reply == NULL) {
            /* Network error → fail-closed */
            hit = 1;
        }
        if (reply != NULL) {
            freeReplyObject(reply);
        }
        redis_pool_release(g_lockout_pool, c);
        return hit;
    }

    lockout_slot_t* s = &g_lockout[lockout_slot_for(ip)];
    pthread_mutex_lock(&g_lockout_mtx);
    int hit = s->in_use && atomic_load(&s->fails) >= g_lockout_fails &&
              time(NULL) - atomic_load(&s->first_fail) < g_lockout_window_s;
    pthread_mutex_unlock(&g_lockout_mtx);
    return hit;
}

/** @brief Record one admin auth failure: dual-count Redis distributed counter and local slot counter (window rolls to reset). */
static void
lockout_fail(const char* ip)
{
    if (ip == NULL) {
        return;
    }

    if (g_lockout_pool != NULL) {
        char redis_key[80];
        snprintf(redis_key, sizeof(redis_key), "aigate:lockout:%s", ip);
        redisContext* c = redis_pool_acquire(g_lockout_pool);
        if (c == NULL) {
            goto local_fail;
        }
        char max_buf[16], win_buf[16];
        snprintf(max_buf, sizeof(max_buf), "%d", g_lockout_fails);
        snprintf(win_buf, sizeof(win_buf), "%d", g_lockout_window_s);
        const char* keys[1] = {redis_key};
        const char* argv[2] = {max_buf, win_buf};
        redisReply* reply =
            redis_eval_sha(c, g_lockout_sha, SCRIPT_ADMIN_LOCKOUT, 1, keys, argv, 2);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
        redis_pool_release(g_lockout_pool, c);
        /* Fall through to also record locally */
    }

local_fail:;
    lockout_slot_t* s = &g_lockout[lockout_slot_for(ip)];
    time_t          now = time(NULL);
    pthread_mutex_lock(&g_lockout_mtx);
    if (!s->in_use || strcmp(s->ip, ip) != 0) {
        s->in_use = 1;
        snprintf(s->ip, sizeof s->ip, "%s", ip);
        atomic_store(&s->fails, 1);
        atomic_store(&s->first_fail, now);
    } else {
        if (now - atomic_load(&s->first_fail) >= g_lockout_window_s) {
            /* window elapsed: restart the counter */
            atomic_store(&s->fails, 0);
            atomic_store(&s->first_fail, now);
        }
        atomic_fetch_add(&s->fails, 1);
    }
    pthread_mutex_unlock(&g_lockout_mtx);
}

/** @brief Clear the local failure count for this IP (called after successful auth). */
static void
lockout_clear(const char* ip)
{
    if (ip == NULL) {
        return;
    }
    lockout_slot_t* s = &g_lockout[lockout_slot_for(ip)];
    pthread_mutex_lock(&g_lockout_mtx);
    if (s->in_use && strcmp(s->ip, ip) == 0) {
        s->in_use = 0;
        atomic_store(&s->fails, 0);
        atomic_store(&s->first_fail, 0);
    }
    pthread_mutex_unlock(&g_lockout_mtx);
}

void
admin_lockout_reset(void)
{
    pthread_mutex_lock(&g_lockout_mtx);
    for (int i = 0; i < LOCKOUT_SLOTS; i++) {
        g_lockout[i].in_use = 0;
        atomic_store(&g_lockout[i].fails, 0);
        atomic_store(&g_lockout[i].first_fail, 0);
    }
    pthread_mutex_unlock(&g_lockout_mtx);
}

void
admin_lockout_set_policy(int max_fails, int window_s)
{
    /* Out-of-range values keep the current policy. */
    if (max_fails < 2 || max_fails > 1000) {
        return;
    }
    if (window_s < 5 || window_s > 3600) {
        return;
    }
    pthread_mutex_lock(&g_lockout_mtx);
    g_lockout_fails = max_fails;
    g_lockout_window_s = window_s;
    pthread_mutex_unlock(&g_lockout_mtx);
}

void
admin_lockout_set_pool(struct redis_pool* pool)
{
    pthread_mutex_lock(&g_lockout_mtx);
    g_lockout_pool = pool;
    if (pool != NULL) {
        redisContext* c = redis_pool_acquire(pool);
        if (c != NULL) {
            redis_script_load(c, SCRIPT_ADMIN_LOCKOUT, g_lockout_sha);
            redis_pool_release(pool, c);
        }
    }
    pthread_mutex_unlock(&g_lockout_mtx);
}

/** @brief Dump @p j as compact JSON into the out params; decref j.
 * @return 0 ok, -1 on dump failure (out_* untouched). */
static int
finish_json(int* status, char** body, size_t* len, int http, json_t* j)
{
    char* packed = json_dumps(j, JSON_COMPACT);
    json_decref(j);
    if (packed == NULL) {
        AIGATE_LOG_ERROR("admin: json dump failed");
        return -1;
    }
    *status = http;
    *body = packed;
    *len = strlen(packed);
    return 0;
}

/** @brief Pack the error body in OpenAI shape `{"error":{"message","type","code"}}` via finish_json.
 *  @return finish_json result. */
static int
finish_error(int* status, char** body, size_t* len, int http, const char* type, const char* message)
{
    json_t* err = json_object();
    json_object_set_new(err, "message", json_string(message));
    json_object_set_new(err, "type", json_string(type));
    json_object_set_new(err, "code", json_integer(http));
    json_t* root = json_object();
    json_object_set_new(root, "error", err);
    return finish_json(status, body, len, http, root);
}

/** @brief Parse @p body (NUL-terminated) into a JSON object.
 *  Returns a ref the caller must decref; NULL on malformed input. */
static json_t*
parse_body(const void* body, size_t body_len)
{
    (void)body_len;
    if (body == NULL) {
        return json_object();
    }
    json_t* j = json_loads((const char*)body, 0, NULL);
    if (j == NULL || !json_is_object(j)) {
        json_decref(j);
        return NULL;
    }
    return j;
}

/** @brief Get a JSON object string field value; return @p fallback when missing/non-string (borrowed pointer, do not free). */
static const char*
jstring(const json_t* obj, const char* field, const char* fallback)
{
    json_t* v = json_object_get(obj, field);
    if (v != NULL && json_is_string(v)) {
        return json_string_value(v);
    }
    return fallback;
}

/** @brief Copy the value of @p field from the query string into @p out. */
static int
query_param(const char* query, const char* field, char* out, size_t cap)
{
    out[0] = '\0';
    if (query == NULL || field == NULL || field[0] == '\0') {
        return 0;
    }
    size_t      flen = strlen(field);
    const char* p = query;
    while (p != NULL && *p != '\0') {
        size_t seglen = strcspn(p, "&");
        if (strncmp(p, field, flen) == 0 && p[flen] == '=') {
            size_t vlen = seglen - flen - 1;
            if (vlen >= cap) {
                vlen = cap - 1;
            }
            memcpy(out, p + flen + 1, vlen);
            out[vlen] = '\0';
            return 0;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return 0;
}

/** @brief Parse query-string pagination params: page (default 1), limit (aliases page_size/size, clamped to [1,1000], default 0 means no paging), offset (converted when page is absent).
 *  @param out_page/out_limit Outputs; always written. */
static void
parse_pagination_params(const char* query, int* out_page, int* out_limit)
{
    char page_str[32] = "";
    char limit_str[32] = "";
    char offset_str[32] = "";

    query_param(query, "page", page_str, sizeof page_str);
    query_param(query, "limit", limit_str, sizeof limit_str);
    if (limit_str[0] == '\0') {
        query_param(query, "page_size", limit_str, sizeof limit_str);
    }
    if (limit_str[0] == '\0') {
        query_param(query, "size", limit_str, sizeof limit_str);
    }
    query_param(query, "offset", offset_str, sizeof offset_str);

    int page = 1;
    int limit = 0;

    if (limit_str[0] != '\0') {
        limit = (int)strtol(limit_str, NULL, 10);
        if (limit < 1) {
            limit = 1;
        }
        if (limit > 1000) {
            limit = 1000;
        }
    }

    if (page_str[0] != '\0') {
        page = (int)strtol(page_str, NULL, 10);
        if (page < 1) {
            page = 1;
        }
    } else if (offset_str[0] != '\0' && limit > 0) {
        long off = strtol(offset_str, NULL, 10);
        if (off < 0) {
            off = 0;
        }
        page = (int)(off / limit) + 1;
    }

    *out_page = page;
    *out_limit = limit;
}

/** @brief Paginate a JSON array: return the original array when limit<=0; otherwise return a new page array and decref the input.
 *  @param out_total Optional, always set to the total. @return Page array (caller owns the decref). */
static json_t*
paginate_json_array(json_t* all_items, int page, int limit, size_t* out_total)
{
    size_t total = json_array_size(all_items);
    if (out_total != NULL) {
        *out_total = total;
    }
    if (limit <= 0) {
        return all_items;
    }

    json_t* paged = json_array();
    size_t  start = (size_t)(page > 0 ? (page - 1) : 0) * (size_t)limit;
    if (start < total) {
        size_t end = start + (size_t)limit;
        if (end > total) {
            end = total;
        }
        for (size_t i = start; i < end; i++) {
            json_t* item = json_array_get(all_items, i);
            json_incref(item);
            json_array_append_new(paged, item);
        }
    }
    json_decref(all_items);
    return paged;
}

/** @brief Write total into the list response root object; append page/limit when paged. */
static void
add_pagination_meta(json_t* root, size_t total, int page, int limit)
{
    json_object_set_new(root, "total", json_integer((json_int_t)total));
    if (limit > 0) {
        json_object_set_new(root, "page", json_integer(page));
        json_object_set_new(root, "limit", json_integer(limit));
    }
}

/** @brief API key plaintext buffer size ("aig_" + 32 hex + NUL). */
#define KEY_PLAIN_CAP 48
/** @brief SHA-256 hex digest buffer size (64 hex + NUL). */
#define KEY_HASH_CAP 65

/** @brief Build a 16-byte random "aig_" + 32 hex plaintext; hash it. */
static int
gen_key_plaintext(char plain[KEY_PLAIN_CAP], char hash_out[KEY_HASH_CAP])
{
    uint8_t raw[16];
    if (RAND_bytes(raw, sizeof raw) != 1) {
        return -1;
    }
    char hex[33];
    for (size_t i = 0; i < sizeof raw; i++) {
        snprintf(hex + 2 * i, 3, "%02x", raw[i]);
    }
    hex[32] = '\0';
    snprintf(plain, KEY_PLAIN_CAP, "aig_%s", hex);
    if (sha256_hex(plain, strlen(plain), hash_out) != 0) {
        return -1;
    }
    return 0;
}

static int
parse_prompt_mode(const json_t* v, int default_mode)
{
    if (v == NULL) {
        return default_mode;
    }
    if (json_is_string(v)) {
        const char* s = json_string_value(v);
        if (strcmp(s, "append") == 0) {
            return 1;
        }
        if (strcmp(s, "override") == 0) {
            return 2;
        }
        return 0; /* prepend */
    }
    if (json_is_integer(v)) {
        int val = (int)json_integer_value(v);
        if (val >= 0 && val <= 2) {
            return val;
        }
    }
    return default_mode;
}

static const char*
prompt_mode_str(int mode)
{
    switch (mode) {
    case 1:
        return "append";
    case 2:
        return "override";
    default:
        return "prepend";
    }
}

/** @brief POST /admin/v1/keys: create an API key, plaintext returned only this once (only the hash is stored).
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
key_create(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    key_rec_t k;
    memset(&k, 0, sizeof k);
    char        name[128] = "unnamed";
    const char* n = jstring(jbody, "name", NULL);
    if (n != NULL && n[0] != '\0') {
        snprintf(name, sizeof name, "%s", n);
    }
    memcpy(k.name, name, sizeof k.name);

    json_t* jal = json_object_get(jbody, "allowed_models");
    if (jal != NULL && json_is_array(jal)) {
        size_t      cnt = json_array_size(jal);
        const char* err_msg = "allowed_models overflow";
        if (cnt > 64) {
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", err_msg);
        }
        k.allowed_models = calloc(cnt > 0 ? cnt : 1, sizeof(char*));
        if (k.allowed_models == NULL) {
            json_decref(jbody);
            return -1;
        }
        for (size_t i = 0; i < cnt; i++) {
            json_t* item = json_array_get(jal, i);
            if (!json_is_string(item)) {
                for (size_t j = 0; j < i; j++) {
                    free(k.allowed_models[j]);
                }
                free(k.allowed_models);
                json_decref(jbody);
                return finish_error(status,
                                    body,
                                    len,
                                    400,
                                    "bad_request",
                                    "allowed_models entries must be strings");
            }
            k.allowed_models[i] = strdup(json_string_value(item));
            if (k.allowed_models[i] == NULL) {
                for (size_t j = 0; j < i; j++) {
                    free(k.allowed_models[j]);
                }
                free(k.allowed_models);
                json_decref(jbody);
                return -1;
            }
        }
        k.n_allowed = (int)cnt;
    }

    json_t* jrate = json_object_get(jbody, "rate_qps");
    if (jrate != NULL && json_is_integer(jrate)) {
        k.rate_qps = (int)json_integer_value(jrate);
    }
    json_t* jquota = json_object_get(jbody, "daily_token_quota");
    if (jquota != NULL && json_is_integer(jquota)) {
        k.daily_token_quota = json_integer_value(jquota);
    }
    json_t* jexp = json_object_get(jbody, "expires_at");
    if (jexp != NULL && json_is_integer(jexp)) {
        k.expires_at = (time_t)json_integer_value(jexp);
        k.has_expiry = 1;
    }
    json_t* jgroup = json_object_get(jbody, "group_id");
    if (jgroup != NULL && json_is_integer(jgroup)) {
        k.group_id = json_integer_value(jgroup);
    }
    k.guardrails_enabled = 1;
    json_t* jgr = json_object_get(jbody, "guardrails_enabled");
    if (jgr != NULL && json_is_boolean(jgr)) {
        k.guardrails_enabled = json_is_true(jgr) ? 1 : 0;
    }
    json_t* jcost_b = json_object_get(jbody, "monthly_cost_budget");
    if (jcost_b != NULL && json_is_number(jcost_b)) {
        k.monthly_cost_budget = json_number_value(jcost_b);
    }
    json_t* jtok_b = json_object_get(jbody, "monthly_token_budget");
    if (jtok_b != NULL && json_is_integer(jtok_b)) {
        k.monthly_token_budget = json_integer_value(jtok_b);
    }
    json_t* jspr = json_object_get(jbody, "system_prompt");
    if (jspr != NULL && json_is_string(jspr)) {
        snprintf(k.system_prompt, sizeof k.system_prompt, "%s", json_string_value(jspr));
    }
    json_t* jpm = json_object_get(jbody, "prompt_mode");
    if (jpm != NULL) {
        k.prompt_mode = parse_prompt_mode(jpm, 0);
    }

    char plain[48], hash[65];
    if (gen_key_plaintext(plain, hash) != 0) {
        key_rec_free(&k);
        json_decref(jbody);
        return -1;
    }
    memcpy(k.key_hash, hash, sizeof k.key_hash);

    long            id = 0;
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             rc = ops->create_key(ops->ctx, &k, &id);
    /* allowlist ownership stays with us; the op stored a copy/joined string */
    for (int i = 0; i < k.n_allowed; i++) {
        free(k.allowed_models[i]);
    }
    free(k.allowed_models);
    json_decref(jbody);
    if (rc == -2) {
        return finish_error(status, body, len, 404, "group_not_found", "group not found");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "key create failed");
    }

    json_t* out = json_object();
    json_object_set_new(out, "key_id", json_integer(id));
    json_object_set_new(out, "plaintext", json_string(plain));
    json_object_set_new(out, "name", json_string(k.name));
    json_object_set_new(out, "group_id", k.group_id > 0 ? json_integer(k.group_id) : json_null());
    json_object_set_new(out, "guardrails_enabled", json_boolean(k.guardrails_enabled));
    json_object_set_new(out, "monthly_cost_budget", json_real(k.monthly_cost_budget));
    json_object_set_new(out, "monthly_token_budget", json_integer(k.monthly_token_budget));
    json_object_set_new(out,
                        "system_prompt",
                        k.system_prompt[0] != '\0' ? json_string(k.system_prompt) : json_null());
    json_object_set_new(out, "prompt_mode", json_string(prompt_mode_str(k.prompt_mode)));
    return finish_json(status, body, len, 201, out);
}

/** @brief GET /admin/v1/keys: list API keys with paging (no plaintext).
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
key_list(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    int req_cap = KEY_LIST_CAP;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    key_rec_t* recs = calloc((size_t)req_cap, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_keys(ops->ctx, recs, req_cap, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "key list failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_t* al = json_array();
        for (int j = 0; j < recs[i].n_allowed; j++) {
            json_array_append_new(al, json_string(recs[i].allowed_models[j]));
        }
        json_object_set_new(o, "key_id", json_integer(recs[i].key_id));
        json_object_set_new(o, "name", json_string(recs[i].name));
        json_object_set_new(o, "key_hash", json_string(recs[i].key_hash));
        json_object_set_new(o, "allowed_models", al);
        json_object_set_new(o, "rate_qps", json_integer(recs[i].rate_qps));
        json_object_set_new(o, "daily_token_quota", json_integer(recs[i].daily_token_quota));
        json_object_set_new(o, "revoked", json_integer(recs[i].revoked));
        if (recs[i].has_expiry) {
            json_object_set_new(o, "expires_at", json_integer((int64_t)recs[i].expires_at));
        }
        json_object_set_new(
            o, "group_id", recs[i].group_id > 0 ? json_integer(recs[i].group_id) : json_null());
        json_object_set_new(o, "guardrails_enabled", json_boolean(recs[i].guardrails_enabled));
        json_object_set_new(o, "monthly_cost_budget", json_real(recs[i].monthly_cost_budget));
        json_object_set_new(o, "monthly_token_budget", json_integer(recs[i].monthly_token_budget));
        json_object_set_new(o,
                            "system_prompt",
                            recs[i].system_prompt[0] != '\0' ? json_string(recs[i].system_prompt)
                                                             : json_null());
        json_object_set_new(o, "prompt_mode", json_string(prompt_mode_str(recs[i].prompt_mode)));
        json_array_append_new(arr, o);
        key_rec_free(&recs[i]);
    }
    free(recs);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "keys", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief Parse "/admin/v1/keys/<id>" → @p key_id. @return 0 ok, -1 bad. */
static int
path_key_id(const char* rest, long* key_id)
{
    char* end = NULL;
    long  v = strtol(rest, &end, 10);
    if (rest[0] == '\0' || end == rest || *end != '\0' || v <= 0) {
        return -1;
    }
    *key_id = v;
    return 0;
}

/** @brief PATCH/PUT /admin/v1/keys/<id>: update a key by numeric id (name/quota/switches etc.).
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
key_patch(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    long id;
    if (path_key_id(rest, &id) != 0) {
        return finish_error(status, body, len, 404, "not_found", "key id not found");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    key_rec_t       existing;
    if (ops->get_key_by_id(ops->ctx, id, &existing) != 0) {
        return finish_error(status, body, len, 404, "not_found", "key not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        key_rec_free(&existing);
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    int       mask = 0;
    key_rec_t k = existing; /* start from the stored record */

    json_t* v;
    v = json_object_get(jbody, "rate_qps");
    if (v != NULL && json_is_integer(v)) {
        k.rate_qps = (int)json_integer_value(v);
        mask |= KMASK_RATE;
    }
    v = json_object_get(jbody, "daily_token_quota");
    if (v != NULL && json_is_integer(v)) {
        k.daily_token_quota = json_integer_value(v);
        mask |= KMASK_QUOTA;
    }
    v = json_object_get(jbody, "allowed_models");
    if (v != NULL) {
        if (!json_is_array(v)) {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "allowed_models must be an array");
        }
        size_t cnt = json_array_size(v);
        if (cnt > 64) {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "allowed_models overflow");
        }
        for (size_t i = 0; i < cnt; i++) {
            json_t* item = json_array_get(v, i);
            if (!json_is_string(item)) {
                key_rec_free(&k);
                json_decref(jbody);
                return finish_error(status,
                                    body,
                                    len,
                                    400,
                                    "bad_request",
                                    "allowed_models entries must be strings");
            }
        }
        /* replace the allowlist */
        for (int i = 0; i < k.n_allowed; i++) {
            free(k.allowed_models[i]);
        }
        free(k.allowed_models);
        k.allowed_models = calloc(cnt > 0 ? cnt : 1, sizeof(char*));
        if (k.allowed_models == NULL) {
            json_decref(jbody);
            return -1;
        }
        for (size_t i = 0; i < cnt; i++) {
            k.allowed_models[i] = strdup(json_string_value(json_array_get(v, i)));
        }
        k.n_allowed = (int)cnt;
        mask |= KMASK_ALLOWLIST;
    }
    v = json_object_get(jbody, "expires_at");
    if (v != NULL) {
        if (json_is_integer(v)) {
            k.expires_at = (time_t)json_integer_value(v);
            k.has_expiry = 1;
        } else if (json_is_null(v)) {
            k.expires_at = 0;
            k.has_expiry = 0;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "expires_at must be integer or null");
        }
        mask |= KMASK_EXPIRY;
    }
    v = json_object_get(jbody, "group_id");
    if (v != NULL) {
        if (json_is_integer(v)) {
            k.group_id = json_integer_value(v);
            mask |= KMASK_GROUP;
        } else if (json_is_null(v)) {
            k.group_id = 0;
            mask |= KMASK_GROUP;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "group_id must be integer or null");
        }
    }
    v = json_object_get(jbody, "guardrails_enabled");
    if (v != NULL) {
        if (json_is_boolean(v)) {
            k.guardrails_enabled = json_is_true(v) ? 1 : 0;
            mask |= KMASK_GUARDRAILS;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "guardrails_enabled must be boolean");
        }
    }
    v = json_object_get(jbody, "monthly_cost_budget");
    if (v != NULL) {
        if (json_is_number(v)) {
            k.monthly_cost_budget = json_number_value(v);
            mask |= KMASK_MONTHLY_COST_BUDGET;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "monthly_cost_budget must be a number");
        }
    }
    v = json_object_get(jbody, "monthly_token_budget");
    if (v != NULL) {
        if (json_is_integer(v)) {
            k.monthly_token_budget = json_integer_value(v);
            mask |= KMASK_MONTHLY_TOKEN_BUDGET;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "monthly_token_budget must be integer");
        }
    }
    v = json_object_get(jbody, "system_prompt");
    if (v != NULL) {
        if (json_is_string(v)) {
            snprintf(k.system_prompt, sizeof k.system_prompt, "%s", json_string_value(v));
            mask |= KMASK_SYSTEM_PROMPT;
        } else if (json_is_null(v)) {
            k.system_prompt[0] = '\0';
            mask |= KMASK_SYSTEM_PROMPT;
        } else {
            key_rec_free(&k);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "system_prompt must be string or null");
        }
    }
    v = json_object_get(jbody, "prompt_mode");
    if (v != NULL) {
        k.prompt_mode = parse_prompt_mode(v, 0);
        mask |= KMASK_PROMPT_MODE;
    }
    json_decref(jbody);

    int rc = ops->update_key(ops->ctx, &k, mask);
    key_rec_free(&k);
    if (rc == -2) {
        return finish_error(status, body, len, 404, "group_not_found", "group not found");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "key update failed");
    }
    auth_key_invalidate(&adm->ac->keys, existing.key_hash);

    json_t* out = json_object();
    json_object_set_new(out, "key_id", json_integer(id));
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief DELETE /admin/v1/keys/<id>: revoke an API key.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
key_revoke(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    long id;
    if (path_key_id(rest, &id) != 0) {
        return finish_error(status, body, len, 404, "not_found", "key id not found");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    key_rec_t       existing;
    if (ops->get_key_by_id(ops->ctx, id, &existing) != 0) {
        return finish_error(status, body, len, 404, "not_found", "key not found");
    }
    int rc = ops->revoke_key(ops->ctx, id);
    key_rec_free(&existing);
    if (rc != 0) {
        return finish_error(status, body, len, 404, "not_found", "key not found");
    }
    /* revoke_key returns -1 when no row was affected; get_key_by_id already
     * proved the row exists, so rc != 0 means a store failure. */
    auth_key_invalidate(&adm->ac->keys, existing.key_hash);
    json_t* out = json_object();
    json_object_set_new(out, "key_id", json_integer(id));
    json_object_set_new(out, "revoked", json_true());
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ models */

static int
parse_targets_array(json_t* jtargets, upstream_target_t* targets, int max_targets, int* n_targets)
{
    if (!json_is_array(jtargets)) {
        return -1;
    }
    size_t sz = json_array_size(jtargets);
    if (sz > (size_t)max_targets) {
        sz = max_targets;
    }
    *n_targets = (int)sz;
    for (size_t i = 0; i < sz; i++) {
        json_t* item = json_array_get(jtargets, i);
        if (!json_is_object(item)) {
            return -1;
        }
        upstream_target_t* tgt = &targets[i];
        memset(tgt, 0, sizeof(*tgt));
        json_t* p = json_object_get(item, "provider");
        if (p && json_is_string(p)) {
            snprintf(tgt->provider, sizeof tgt->provider, "%s", json_string_value(p));
        } else {
            snprintf(tgt->provider, sizeof tgt->provider, "openai");
        }
        json_t* ep = json_object_get(item, "endpoint");
        if (ep && json_is_string(ep)) {
            snprintf(tgt->endpoint, sizeof tgt->endpoint, "%s", json_string_value(ep));
        }
        json_t* key = json_object_get(item, "upstream_key_ref");
        if (!key) {
            key = json_object_get(item, "upstream_key");
        }
        if (key && json_is_string(key)) {
            snprintf(
                tgt->upstream_key_ref, sizeof tgt->upstream_key_ref, "%s", json_string_value(key));
        }
        json_t* w = json_object_get(item, "weight");
        tgt->weight = (w && json_is_integer(w)) ? (int)json_integer_value(w) : 1;
        if (tgt->weight <= 0) {
            tgt->weight = 1;
        }

        json_t* pr = json_object_get(item, "priority");
        tgt->priority = (pr && json_is_integer(pr)) ? (int)json_integer_value(pr) : 0;

        json_t* mc = json_object_get(item, "max_concurrent");
        tgt->max_concurrent = (mc && json_is_integer(mc) && json_integer_value(mc) >= 0)
                                  ? (int)json_integer_value(mc)
                                  : 0;
    }
    return 0;
}

/** @brief POST /admin/v1/models: create a model route record.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
model_create(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    model_rec_t m;
    memset(&m, 0, sizeof m);

    const char* name = jstring(jbody, "name", NULL);
    if (name == NULL || name[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "name is required");
    }
    if (strlen(name) >= 128) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "model name too long (max 127 chars)");
    }
    snprintf(m.name, sizeof m.name, "%s", name);

    json_t* jtargets = json_object_get(jbody, "targets");
    if (jtargets != NULL) {
        if (parse_targets_array(jtargets, m.targets, 8, &m.n_targets) != 0) {
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "invalid targets array");
        }
    }

    const char* def_prov =
        (m.n_targets > 0 && m.targets[0].provider[0]) ? m.targets[0].provider : "openai";
    const char* def_endp =
        (m.n_targets > 0 && m.targets[0].endpoint[0]) ? m.targets[0].endpoint : "";
    const char* def_kref =
        (m.n_targets > 0 && m.targets[0].upstream_key_ref[0]) ? m.targets[0].upstream_key_ref : "";

    snprintf(m.provider,
             sizeof m.provider,
             "%.*s",
             (int)sizeof m.provider - 1,
             jstring(jbody, "provider", def_prov));
    snprintf(m.endpoint, sizeof m.endpoint, "%s", jstring(jbody, "endpoint", def_endp));
    snprintf(m.upstream_key_ref,
             sizeof m.upstream_key_ref,
             "%.*s",
             (int)sizeof m.upstream_key_ref - 1,
             jstring(jbody, "upstream_key_ref", def_kref));
    snprintf(m.lb_policy, sizeof m.lb_policy, "%s", jstring(jbody, "lb_policy", "priority"));

    char    fallback_model_buf[128] = {0};
    json_t* jfb = json_object_get(jbody, "fallback_model");
    if (jfb != NULL && json_is_string(jfb)) {
        snprintf(fallback_model_buf, sizeof(fallback_model_buf), "%s", json_string_value(jfb));
    }

    json_t* jparams = json_object_get(jbody, "default_params");
    json_t* params_obj = NULL;
    if (jparams != NULL && json_is_object(jparams)) {
        params_obj = json_deep_copy(jparams);
    } else {
        params_obj = json_object();
    }
    if (fallback_model_buf[0] != '\0') {
        json_object_set_new(params_obj, "fallback_model", json_string(fallback_model_buf));
    }
    char* packed = json_dumps(params_obj, JSON_COMPACT);
    json_decref(params_obj);
    if (packed == NULL) {
        json_decref(jbody);
        return finish_error(status, body, len, 500, "internal_error", "json encode failed");
    }
    if (strlen(packed) >= sizeof m.default_params_json) {
        /* A truncated JSONB blob silently poisons the model row; reject. */
        free(packed);
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "default_params too large");
    }
    snprintf(m.default_params_json, sizeof m.default_params_json, "%s", packed);
    free(packed);

    json_t* jpricing = json_object_get(jbody, "pricing");
    if (jpricing != NULL) {
        if (!json_is_object(jpricing)) {
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "pricing must be an object");
        }
        char* packed = json_dumps(jpricing, JSON_COMPACT);
        if (packed != NULL) {
            snprintf(m.pricing_json, sizeof m.pricing_json, "%s", packed);
            free(packed);
        }
    } else {
        snprintf(m.pricing_json, sizeof m.pricing_json, "{}");
    }
    json_t* jspr = json_object_get(jbody, "system_prompt");
    if (jspr != NULL && json_is_string(jspr)) {
        snprintf(m.system_prompt, sizeof m.system_prompt, "%s", json_string_value(jspr));
    }
    json_t* jpm = json_object_get(jbody, "prompt_mode");
    if (jpm != NULL) {
        m.prompt_mode = parse_prompt_mode(jpm, 0);
    }
    json_t* jhedged = json_object_get(jbody, "hedged_enabled");
    if (jhedged != NULL && json_is_boolean(jhedged)) {
        m.hedged_enabled = json_is_true(jhedged);
    }
    json_t* jhdelay = json_object_get(jbody, "hedged_delay_ms");
    if (jhdelay != NULL && json_is_integer(jhdelay)) {
        int delay = (int)json_integer_value(jhdelay);
        if (delay >= 0) {
            m.hedged_delay_ms = delay;
        }
    }
    json_t* jhbudget = json_object_get(jbody, "hedge_budget_pct");
    if (jhbudget != NULL && json_is_integer(jhbudget)) {
        int budget = (int)json_integer_value(jhbudget);
        if (budget >= 0 && budget <= 100) {
            m.hedge_budget_pct = budget;
        }
    } else {
        m.hedge_budget_pct = 15;
    }
    json_t* jmc = json_object_get(jbody, "max_concurrent");
    if (jmc != NULL && json_is_integer(jmc)) {
        int mc = (int)json_integer_value(jmc);
        if (mc >= 0) {
            m.max_concurrent = mc;
        }
    }
    m.enabled = 1;

    int rc = pg_store_ops(adm->ps)->create_key != NULL
                 ? pg_store_ops(adm->ps)->create_model(pg_store_ops(adm->ps)->ctx, &m)
                 : -1;
    json_decref(jbody);
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "model create failed");
    }
    model_router_invalidate(adm->ac->router, m.name);
    if (fallback_model_buf[0] != '\0') {
        if (adm->ac != NULL && adm->ac->cb != NULL) {
            cb_configure_sla(adm->ac->cb, m.name, 3000, 6000, 10, 0.40f, fallback_model_buf);
        }
    }

    json_t* out = json_object();
    json_object_set_new(out, "name", json_string(m.name));
    json_object_set_new(out, "created", json_true());
    json_object_set_new(out, "hedged_enabled", json_boolean(m.hedged_enabled));
    json_object_set_new(out, "hedged_delay_ms", json_integer(m.hedged_delay_ms));
    json_object_set_new(out, "hedge_budget_pct", json_integer(m.hedge_budget_pct));
    json_object_set_new(out, "max_concurrent", json_integer(m.max_concurrent));
    json_object_set_new(out,
                        "system_prompt",
                        m.system_prompt[0] != '\0' ? json_string(m.system_prompt) : json_null());
    json_object_set_new(out, "prompt_mode", json_string(prompt_mode_str(m.prompt_mode)));
    return finish_json(status, body, len, 201, out);
}

/** @brief GET /admin/v1/models: list model routes with paging.
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
model_list(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    int req_cap = MODEL_LIST_CAP;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    model_rec_t* recs = calloc((size_t)req_cap, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_models(ops->ctx, recs, req_cap, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "model list failed");
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "model_name", json_string(recs[i].name));
        json_object_set_new(o, "name", json_string(recs[i].name));
        json_object_set_new(o, "provider", json_string(recs[i].provider));
        json_object_set_new(o, "endpoint", json_string(recs[i].endpoint));
        json_object_set_new(o, "upstream_key_ref", json_string(recs[i].upstream_key_ref));
        json_object_set_new(o, "enabled", json_integer(recs[i].enabled));
        json_object_set_new(o, "default_params", json_string(recs[i].default_params_json));
        json_object_set_new(
            o,
            "lb_policy",
            json_string(recs[i].lb_policy[0] != '\0' ? recs[i].lb_policy : "priority"));
        json_object_set_new(o, "hedged_enabled", json_boolean(recs[i].hedged_enabled));
        json_object_set_new(o, "hedged_delay_ms", json_integer(recs[i].hedged_delay_ms));
        json_object_set_new(o, "hedge_budget_pct", json_integer(recs[i].hedge_budget_pct));

        json_error_t jerr;
        json_t*      jp =
            json_loads(recs[i].pricing_json[0] != '\0' ? recs[i].pricing_json : "{}", 0, &jerr);
        if (jp != NULL) {
            json_object_set_new(o, "pricing", jp);
        } else {
            json_object_set_new(o, "pricing", json_object());
        }

        json_t* tgts_arr = json_array();
        for (int t = 0; t < recs[i].n_targets; t++) {
            upstream_target_t* tgt = &recs[i].targets[t];
            json_t*            to = json_object();
            json_object_set_new(to, "provider", json_string(tgt->provider));
            json_object_set_new(to, "endpoint", json_string(tgt->endpoint));
            json_object_set_new(to, "upstream_key_ref", json_string(tgt->upstream_key_ref));
            json_object_set_new(to, "weight", json_integer(tgt->weight));
            json_object_set_new(to, "priority", json_integer(tgt->priority));
            json_object_set_new(to, "max_concurrent", json_integer(tgt->max_concurrent));

            const char* cb_state_str = "closed";
            if (adm->ac != NULL && adm->ac->cb != NULL) {
                cb_state_t st = cb_get_state(adm->ac->cb, recs[i].name, tgt->endpoint);
                if (st == CB_OPEN) {
                    cb_state_str = "open";
                } else if (st == CB_HALF_OPEN) {
                    cb_state_str = "half_open";
                }
            }
            json_object_set_new(to, "cb_state", json_string(cb_state_str));
            json_array_append_new(tgts_arr, to);
        }
        json_object_set_new(o, "targets", tgts_arr);
        json_object_set_new(o, "max_concurrent", json_integer(recs[i].max_concurrent));
        json_object_set_new(o,
                            "system_prompt",
                            recs[i].system_prompt[0] != '\0' ? json_string(recs[i].system_prompt)
                                                             : json_null());
        json_object_set_new(o, "prompt_mode", json_string(prompt_mode_str(recs[i].prompt_mode)));

        json_array_append_new(arr, o);
        model_rec_free(&recs[i]);
    }
    free(recs);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "models", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief PATCH/PUT /admin/v1/models/<name>: update a route by model name (endpoint/key reference/switches etc.).
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
model_patch(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    if (rest[0] == '\0') {
        return finish_error(status, body, len, 404, "not_found", "model name not found");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    model_rec_t     existing;
    if (ops->get_model(ops->ctx, rest, &existing) != 0) {
        return finish_error(status, body, len, 404, "not_found", "model not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        model_rec_free(&existing);
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    int         mask = 0;
    model_rec_t m = existing;

    json_t* v = json_object_get(jbody, "endpoint");
    if (v != NULL && json_is_string(v)) {
        snprintf(m.endpoint, sizeof m.endpoint, "%s", json_string_value(v));
        mask |= MMASK_ENDPOINT;
    }
    v = json_object_get(jbody, "default_params");
    if (v != NULL && json_is_object(v)) {
        char* packed = json_dumps(v, JSON_COMPACT);
        if (packed == NULL) {
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 500, "internal_error", "json encode failed");
        }
        if (strlen(packed) >= sizeof m.default_params_json) {
            /* Oversized blob would truncate the JSONB; reject, leave mask
             * clear so the stored row is untouched. */
            free(packed);
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "default_params too large");
        }
        snprintf(m.default_params_json, sizeof m.default_params_json, "%s", packed);
        free(packed);
        mask |= MMASK_PARAMS;
    }
    v = json_object_get(jbody, "upstream_key_ref");
    if (v != NULL && json_is_string(v)) {
        snprintf(m.upstream_key_ref, sizeof m.upstream_key_ref, "%s", json_string_value(v));
        mask |= MMASK_KEYREF;
    }
    v = json_object_get(jbody, "enabled");
    if (v != NULL && json_is_boolean(v)) {
        m.enabled = json_is_true(v);
        mask |= MMASK_ENABLED;
    }
    v = json_object_get(jbody, "lb_policy");
    if (v != NULL && json_is_string(v)) {
        snprintf(m.lb_policy, sizeof m.lb_policy, "%s", json_string_value(v));
        mask |= MMASK_LB_POLICY;
    }
    v = json_object_get(jbody, "targets");
    if (v != NULL) {
        if (parse_targets_array(v, m.targets, 8, &m.n_targets) != 0) {
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "invalid targets array");
        }
        mask |= MMASK_TARGETS;
    }
    v = json_object_get(jbody, "pricing");
    if (v != NULL) {
        if (!json_is_object(v)) {
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "pricing must be an object");
        }
        char* packed = json_dumps(v, JSON_COMPACT);
        if (packed == NULL) {
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 500, "internal_error", "json encode failed");
        }
        if (strlen(packed) >= sizeof m.pricing_json) {
            free(packed);
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(status, body, len, 400, "bad_request", "pricing too large");
        }
        snprintf(m.pricing_json, sizeof m.pricing_json, "%s", packed);
        free(packed);
        mask |= MMASK_PRICING;
    }
    v = json_object_get(jbody, "system_prompt");
    if (v != NULL) {
        if (json_is_string(v)) {
            snprintf(m.system_prompt, sizeof m.system_prompt, "%s", json_string_value(v));
            mask |= MMASK_SYSTEM_PROMPT;
        } else if (json_is_null(v)) {
            m.system_prompt[0] = '\0';
            mask |= MMASK_SYSTEM_PROMPT;
        } else {
            model_rec_free(&existing);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "system_prompt must be string or null");
        }
    }
    v = json_object_get(jbody, "prompt_mode");
    if (v != NULL) {
        m.prompt_mode = parse_prompt_mode(v, 0);
        mask |= MMASK_PROMPT_MODE;
    }
    v = json_object_get(jbody, "hedged_enabled");
    if (v != NULL && json_is_boolean(v)) {
        m.hedged_enabled = json_is_true(v);
        mask |= MMASK_HEDGED_ENABLED;
    }
    v = json_object_get(jbody, "hedged_delay_ms");
    if (v != NULL && json_is_integer(v)) {
        int delay = (int)json_integer_value(v);
        if (delay >= 0) {
            m.hedged_delay_ms = delay;
            mask |= MMASK_HEDGED_DELAY;
        }
    }
    v = json_object_get(jbody, "hedge_budget_pct");
    if (v != NULL && json_is_integer(v)) {
        int budget = (int)json_integer_value(v);
        if (budget >= 0 && budget <= 100) {
            m.hedge_budget_pct = budget;
            mask |= MMASK_HEDGE_BUDGET;
        }
    }
    v = json_object_get(jbody, "max_concurrent");
    if (v != NULL && json_is_integer(v)) {
        int mc = (int)json_integer_value(v);
        if (mc >= 0) {
            m.max_concurrent = mc;
            mask |= MMASK_MAX_CONCURRENT;
        }
    }
    json_decref(jbody);

    int rc = ops->update_model(ops->ctx, &m, mask);
    model_rec_free(&m);
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "model update failed");
    }
    model_router_invalidate(adm->ac->router, m.name);

    json_t* out = json_object();
    json_object_set_new(out, "name", json_string(m.name));
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief DELETE /admin/v1/models/<name>: delete a route by model name.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
model_delete(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    if (rest[0] == '\0') {
        return finish_error(status, body, len, 404, "not_found", "model name not found");
    }
    int rc = pg_store_ops(adm->ps)->delete_model(pg_store_ops(adm->ps)->ctx, rest);
    if (rc != 0) {
        return finish_error(status, body, len, 404, "not_found", "model not found");
    }
    model_router_invalidate(adm->ac->router, rest);
    json_t* out = json_object();
    json_object_set_new(out, "name", json_string(rest));
    json_object_set_new(out, "deleted", json_true());
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ providers */

static void
mask_api_key(
    const char* raw_or_ref, char* out, size_t out_cap, const uint8_t* master, int have_master)
{
    char plain[1024];
    plain[0] = '\0';
    if (raw_or_ref == NULL || raw_or_ref[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (strncmp(raw_or_ref, "pg:", 3) == 0) {
        if (have_master && master != NULL) {
            secret_decrypt(master, raw_or_ref + 3, plain, sizeof plain, NULL);
        }
    } else {
        snprintf(plain, sizeof plain, "%s", raw_or_ref);
    }
    size_t len = strlen(plain);
    if (len == 0) {
        snprintf(out, out_cap, "%s", raw_or_ref[0] != '\0' ? "••••••••" : "");
        return;
    }
    if (len <= 8) {
        snprintf(out, out_cap, "••••••••");
        return;
    }
    char   prefix[8];
    size_t pre_len = (len > 7 && strncmp(plain, "sk-", 3) == 0) ? 3 : 2;
    memcpy(prefix, plain, pre_len);
    prefix[pre_len] = '\0';
    const char* suffix = plain + (len - 4);
    snprintf(out, out_cap, "%s••••%s", prefix, suffix);
}

/** @brief Normalize an upstream key for storage: `env:`/`pg:` references stored as-is; plaintext preferably encrypted into `pg:`; without master, plaintext accepted only under allow_plaintext_keys.
 *  @return 0 with @p out_key written; -1 storage refused (out emptied). */
static int
process_api_key_for_storage(admin_ctx_t* adm, const char* input_key, char* out_key, size_t out_sz)
{
    if (input_key == NULL || input_key[0] == '\0') {
        out_key[0] = '\0';
        return 0;
    }
    if (strncmp(input_key, "env:", 4) == 0 || strncmp(input_key, "pg:", 3) == 0) {
        snprintf(out_key, out_sz, "%s", input_key);
        return 0;
    }
    if (adm->ac != NULL && adm->ac->router != NULL && adm->ac->router->have_master) {
        char enc[1024];
        if (secret_encrypt(
                adm->ac->router->master, input_key, strlen(input_key), enc, sizeof enc) == 0) {
            /* "pg:" + enc + NUL: enc must fit out_sz-4 or the value is unusable; fall through to plaintext copy. Width caps -Werror=format-truncation. */
            if (strlen(enc) + 4 <= out_sz) {
                snprintf(out_key, out_sz, "pg:%.*s", (int)(out_sz - 4), enc);
                return 0;
            }
        }
    }
    /* Direct plaintext key: only accepted when explicitly allowed */
    if (!adm->allow_plaintext_keys) {
        out_key[0] = '\0';
        return -1;
    }
    snprintf(out_key, out_sz, "%s", input_key);
    return 0;
}

/** @brief Convert a JSON string array into a strdup string array (skip empty strings; caller frees each item then the array).
 *  @return 0 on success (non-array/empty counts as 0 items); -1 when over 256 items or on allocation failure. */
static int
parse_provider_models_json(const json_t* jarr, char*** out_models, int* out_n)
{
    *out_models = NULL;
    *out_n = 0;
    if (jarr == NULL || !json_is_array(jarr)) {
        return 0;
    }
    size_t sz = json_array_size(jarr);
    if (sz == 0) {
        return 0;
    }
    if (sz > 256) {
        return -1;
    }
    char** arr = calloc(sz, sizeof(char*));
    if (arr == NULL) {
        return -1;
    }
    size_t  idx;
    json_t* item;
    int     count = 0;
    json_array_foreach(jarr, idx, item)
    {
        if (json_is_string(item)) {
            const char* s = json_string_value(item);
            if (s != NULL && s[0] != '\0') {
                arr[count] = strdup(s);
                if (arr[count] == NULL) {
                    for (int j = 0; j < count; j++) {
                        free(arr[j]);
                    }
                    free(arr);
                    return -1;
                }
                count++;
            }
        }
    }
    *out_models = arr;
    *out_n = count;
    return 0;
}

/** @brief Sync a provider model list into the models table: update endpoint/key/switches for existing rows, create missing ones per the priority policy, and refresh the route cache.
 *  @return Number of models that failed to sync (0 means all succeeded). */
static int
sync_provider_models(admin_ctx_t* adm, const provider_rec_t* p)
{
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             failed = 0;
    for (int i = 0; i < p->n_models; i++) {
        const char* m_name = p->models[i];
        if (m_name == NULL || m_name[0] == '\0') {
            continue;
        }
        model_rec_t existing;
        memset(&existing, 0, sizeof existing);
        if (ops->get_model(ops->ctx, m_name, &existing) == 0) {
            copy_field(existing.endpoint, sizeof existing.endpoint, p->endpoint);
            copy_field(existing.upstream_key_ref, sizeof existing.upstream_key_ref, p->api_key);
            existing.enabled = p->enabled;
            if (p->provider_type[0] != '\0') {
                copy_field(existing.provider, sizeof existing.provider, p->provider_type);
            }
            if (existing.n_targets > 0) {
                copy_field(
                    existing.targets[0].endpoint, sizeof existing.targets[0].endpoint, p->endpoint);
                copy_field(existing.targets[0].upstream_key_ref,
                           sizeof existing.targets[0].upstream_key_ref,
                           p->api_key);
                if (p->provider_type[0] != '\0') {
                    copy_field(existing.targets[0].provider,
                               sizeof existing.targets[0].provider,
                               p->provider_type);
                }
                if (ops->update_model(ops->ctx,
                                      &existing,
                                      MMASK_ENDPOINT | MMASK_KEYREF | MMASK_ENABLED |
                                          MMASK_TARGETS) != 0) {
                    failed++;
                }
            } else if (ops->update_model(ops->ctx,
                                         &existing,
                                         MMASK_ENDPOINT | MMASK_KEYREF | MMASK_ENABLED) != 0) {
                failed++;
            }
            model_rec_free(&existing);
        } else {
            model_rec_t m;
            memset(&m, 0, sizeof m);
            copy_field(m.name, sizeof m.name, m_name);
            copy_field(m.provider,
                       sizeof m.provider,
                       p->provider_type[0] != '\0' ? p->provider_type : "openai");
            copy_field(m.endpoint, sizeof m.endpoint, p->endpoint);
            copy_field(m.upstream_key_ref, sizeof m.upstream_key_ref, p->api_key);
            copy_field(m.lb_policy, sizeof m.lb_policy, "priority");
            m.enabled = p->enabled;
            if (ops->create_model(ops->ctx, &m) != 0) {
                failed++;
            }
        }
        if (adm->ac != NULL && adm->ac->router != NULL) {
            model_router_invalidate(adm->ac->router, m_name);
        }
    }
    return failed;
}

/** @brief POST /admin/v1/providers: create an upstream provider and sync its model list.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
provider_create(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    const char* name = jstring(jbody, "name", NULL);
    const char* endpoint = jstring(jbody, "endpoint", NULL);
    if (name == NULL || name[0] == '\0' || endpoint == NULL || endpoint[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "name and endpoint required");
    }

    provider_rec_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.name, sizeof p.name, "%s", name);
    snprintf(p.endpoint, sizeof p.endpoint, "%s", endpoint);

    const char* ptype = jstring(jbody, "provider_type", NULL);
    if (ptype == NULL || ptype[0] == '\0') {
        ptype = jstring(jbody, "provider", "openai");
    }
    snprintf(p.provider_type, sizeof p.provider_type, "%s", ptype);

    const char* key = jstring(jbody, "api_key", "");
    if (process_api_key_for_storage(adm, key, p.api_key, sizeof p.api_key) != 0) {
        json_decref(jbody);
        return finish_error(status,
                            body,
                            len,
                            400,
                            "provider_key_required",
                            "set AIGATE_MASTER_KEY to encrypt provider keys, or enable "
                            "AIGATE_ALLOW_PLAINTEXT_KEYS=1");
    }

    json_t* jenabled = json_object_get(jbody, "enabled");
    p.enabled = (jenabled == NULL || json_is_true(jenabled));

    json_t* jmodels = json_object_get(jbody, "models");
    if (parse_provider_models_json(jmodels, &p.models, &p.n_models) != 0) {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "invalid models array");
    }
    json_decref(jbody);

    const pg_ops_t* ops = pg_store_ops(adm->ps);
    long            new_id = 0;
    if (ops->create_provider(ops->ctx, &p, &new_id) != 0) {
        provider_rec_free(&p);
        return finish_error(
            status, body, len, 400, "bad_request", "provider create failed (duplicate name?)");
    }
    p.id = new_id;

    /* Auto-sync models into models table */
    int sf = sync_provider_models(adm, &p);

    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(new_id));
    json_object_set_new(out, "name", json_string(p.name));
    json_object_set_new(out, "created", json_true());
    json_object_set_new(out, "sync_failed", json_integer(sf));
    provider_rec_free(&p);
    return finish_json(status, body, len, 201, out);
}

/** @brief GET /admin/v1/providers: list upstream providers with paging.
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
provider_list(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    int req_cap = PROVIDER_LIST_CAP;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    provider_rec_t* recs = calloc((size_t)req_cap, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_providers(ops->ctx, recs, req_cap, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "provider list failed");
    }

    const uint8_t* master = NULL;
    int            have_master = 0;
    if (adm->ac != NULL && adm->ac->router != NULL && adm->ac->router->have_master) {
        master = adm->ac->router->master;
        have_master = 1;
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "id", json_integer(recs[i].id));
        json_object_set_new(o, "name", json_string(recs[i].name));
        json_object_set_new(o, "provider_type", json_string(recs[i].provider_type));
        json_object_set_new(o, "endpoint", json_string(recs[i].endpoint));

        char masked[128];
        mask_api_key(recs[i].api_key, masked, sizeof masked, master, have_master);
        json_object_set_new(o, "api_key", json_string(masked));
        json_object_set_new(o, "has_key", json_boolean(recs[i].api_key[0] != '\0'));
        json_object_set_new(o, "enabled", json_integer(recs[i].enabled));
        json_object_set_new(o, "created_at", json_integer((json_int_t)recs[i].created_at));

        json_t* marr = json_array();
        for (int m = 0; m < recs[i].n_models; m++) {
            json_array_append_new(marr, json_string(recs[i].models[m]));
        }
        json_object_set_new(o, "models", marr);

        json_array_append_new(arr, o);
        provider_rec_free(&recs[i]);
    }
    free(recs);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "providers", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief PATCH/PUT /admin/v1/providers/<id>: update a provider by numeric id (changing the model list triggers a sync).
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
provider_patch(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    if (rest[0] == '\0') {
        return finish_error(status, body, len, 404, "not_found", "provider id not found");
    }
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid provider id");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    provider_rec_t  existing;
    if (ops->get_provider(ops->ctx, id, &existing) != 0) {
        return finish_error(status, body, len, 404, "not_found", "provider not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        provider_rec_free(&existing);
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    int            mask = 0;
    provider_rec_t p = existing;

    json_t* v = json_object_get(jbody, "provider_type");
    if (v != NULL && json_is_string(v)) {
        snprintf(p.provider_type, sizeof p.provider_type, "%s", json_string_value(v));
        mask |= PMASK_TYPE;
    }
    v = json_object_get(jbody, "endpoint");
    if (v != NULL && json_is_string(v)) {
        snprintf(p.endpoint, sizeof p.endpoint, "%s", json_string_value(v));
        mask |= PMASK_ENDPOINT;
    }
    v = json_object_get(jbody, "api_key");
    if (v != NULL && json_is_string(v)) {
        const char* raw_key = json_string_value(v);
        /* If user provided a new key and didn't just submit masked dots */
        if (strstr(raw_key, "••••") == NULL && raw_key[0] != '\0') {
            if (process_api_key_for_storage(adm, raw_key, p.api_key, sizeof p.api_key) != 0) {
                provider_rec_free(&p);
                json_decref(jbody);
                return finish_error(status,
                                    body,
                                    len,
                                    400,
                                    "provider_key_required",
                                    "set AIGATE_MASTER_KEY to encrypt provider keys, or enable "
                                    "AIGATE_ALLOW_PLAINTEXT_KEYS=1");
            }
            mask |= PMASK_API_KEY;
        }
    }
    v = json_object_get(jbody, "enabled");
    if (v != NULL && json_is_boolean(v)) {
        p.enabled = json_is_true(v);
        mask |= PMASK_ENABLED;
    }
    v = json_object_get(jbody, "models");
    if (v != NULL && json_is_array(v)) {
        char** new_models = NULL;
        int    n_new = 0;
        if (parse_provider_models_json(v, &new_models, &n_new) == 0) {
            for (int i = 0; i < p.n_models; i++) {
                free(p.models[i]);
            }
            free(p.models);
            p.models = new_models;
            p.n_models = n_new;
            mask |= PMASK_MODELS;
        }
    }
    json_decref(jbody);

    int rc = ops->update_provider(ops->ctx, &p, mask);
    if (rc != 0) {
        provider_rec_free(&p);
        return finish_error(status, body, len, 500, "internal_error", "provider update failed");
    }

    /* Auto-sync models to models table */
    int sf = sync_provider_models(adm, &p);

    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(p.id));
    json_object_set_new(out, "name", json_string(p.name));
    json_object_set_new(out, "updated", json_true());
    json_object_set_new(out, "sync_failed", json_integer(sf));
    provider_rec_free(&p);
    return finish_json(status, body, len, 200, out);
}

/* Deleting a provider leaves its auto-synced models rows in place (P3-3):
 * they still route by their own endpoint/upstream_key_ref, so cleanup of
 * orphan routes is manual until a cascade delete lands (follow-up). */
static int
provider_delete(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    if (rest[0] == '\0') {
        return finish_error(status, body, len, 404, "not_found", "provider id not found");
    }
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid provider id");
    }
    int rc = pg_store_ops(adm->ps)->delete_provider(pg_store_ops(adm->ps)->ctx, id);
    if (rc != 0) {
        return finish_error(status, body, len, 404, "not_found", "provider not found");
    }
    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    json_object_set_new(out, "deleted", json_true());
    return finish_json(status, body, len, 200, out);
}

/* P1-4: resolve a provider api_key ref for the probe (mirrors
 * model_router.c resolve_single_key semantics):
 *   ""      → no auth (local endpoints)
 *   "env:X" → getenv, 400 key_unresolvable when unset/empty
 *   "pg:Y"  → secret_decrypt, 400 key_unresolvable without master or on failure
 *   plain   → used as-is (allow_plaintext_keys era rows) */
static int
provider_probe_resolve_key(admin_ctx_t* adm, const char* key_ref, char* out_key, size_t out_sz)
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
    snprintf(out_key, out_sz, "%s", key_ref);
    return 0;
}

/* P1-4: only "<digits>/test" subpaths enter the probe; everything else
 * keeps the existing 404 fall-through. */
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

/* P1-4: one-shot GET /models probe of a single provider.
 * The probe itself always yields an admin 200 with a verdict in the body;
 * upstream failures are diagnostic data, not admin errors. */
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
        return finish_error(
            status, body, len, 400, "probe_unsupported", "no adapter supports provider_type");
    }

    char key[1080];
    if (provider_probe_resolve_key(adm, rec.api_key, key, sizeof key) != 0) {
        provider_rec_free(&rec);
        return finish_error(
            status, body, len, 400, "key_unresolvable", "cannot resolve provider api_key ref");
    }

    char auth_value[1120];
    if (key[0] != '\0') {
        snprintf(auth_value, sizeof auth_value, "%s%s", plan.bearer ? "Bearer " : "", key);
    } else {
        auth_value[0] = '\0';
    }

    const char* hdr_name = key[0] != '\0' ? plan.auth_header : NULL;
    const char* hdr_value = key[0] != '\0' ? auth_value : NULL;
    const char* extra_name = plan.extra_header[0] != '\0' ? plan.extra_header : NULL;
    const char* extra_value = plan.extra_header[0] != '\0' ? "2023-06-01" : NULL;

    int  us = 0;
    long lat_ns = 0;
    int  rc = upstream_probe(
        plan.url, hdr_name, hdr_value, extra_name, extra_value, 10000L, &us, &lat_ns);

    const char* verdict = "unreachable";
    if (rc == 0) {
        verdict = (us >= 200 && us < 400)    ? "ok"
                  : (us == 401 || us == 403) ? "key_invalid"
                  : us == 404                ? "endpoint_unverified"
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
    if (adm->hp != NULL) {
        health_prober_record_result(adm->hp,
                                    id,
                                    rec.name,
                                    rec.endpoint,
                                    rec.provider_type,
                                    us,
                                    (long)(lat_ns / 1000000L),
                                    rc);
    }
    provider_rec_free(&rec);
    return finish_json(status, body, len, 200, o);
}

/** @brief GET /admin/v1/providers/health: full probe snapshot; returns empty providers when the prober is uninitialized.
 *  @return 0 with status/body filled; -1 only on serialization failure. */
static int
provider_health_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    if (adm->hp == NULL) {
        *status = 200;
        *body = strdup("{\"providers\":[],\"total\":0,\"healthy\":0,\"degraded\":0,\"down\":0,"
                       "\"paused\":0,\"checked_at\":0}");
        *len = *body ? strlen(*body) : 0;
        return 0;
    }
    char* json_str = health_prober_to_json(adm->hp);
    if (json_str == NULL) {
        return finish_error(
            status, body, len, 500, "internal_error", "health serialization failed");
    }
    *status = 200;
    *body = json_str;
    *len = strlen(json_str);
    return 0;
}

/** @brief POST /admin/v1/providers/probe: trigger one full probe immediately and return the snapshot (503 without prober).
 *  @return 0 with status/body filled; -1 only on serialization failure. */
static int
provider_probe_trigger(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    if (adm->hp == NULL) {
        return finish_error(status, body, len, 503, "unavailable", "health prober not initialized");
    }
    health_prober_probe_all(adm->hp);
    char* json_str = health_prober_to_json(adm->hp);
    if (json_str == NULL) {
        return finish_error(
            status, body, len, 500, "internal_error", "health serialization failed");
    }
    *status = 200;
    *body = json_str;
    *len = strlen(json_str);
    return 0;
}

/* ------------------------------------------------------------ usage */

/** @brief "YYYY-MM-DD" (UTC) or ""/"today" → UTC-midnight time_t. */
static int
parse_day(const char* s, time_t* out)
{
    if (s == NULL || s[0] == '\0' || strcmp(s, "today") == 0) {
        time_t now = time(NULL);
        *out = now - (now % 86400);
        return 0;
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (strptime(s, "%Y-%m-%d", &tm) == NULL) {
        return -1;
    }
    tm.tm_isdst = 0;
    *out = timegm(&tm);
    return 0;
}

/** @brief GET /admin/v1/usage?key=&model=&from=&to=: query aggregated usage by key/model/date range (dates go through parse_day).
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
usage_query(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    char key[32] = "", model[128] = "", from[16] = "", to[16] = "";
    query_param(query, "key", key, sizeof key);
    query_param(query, "model", model, sizeof model);
    query_param(query, "from", from, sizeof from);
    query_param(query, "to", to, sizeof to);

    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    char* kend = NULL;
    long  key_id = strtol(key, &kend, 10);
    if (key[0] != '\0' && (kend == key || key_id <= 0)) {
        return finish_error(status, body, len, 400, "bad_request", "bad ?key=<id>");
    }
    time_t t_from, t_to;
    if (from[0] == '\0') {
        time_t now = time(NULL);
        t_from = now - (now % 86400) - 6 * 86400;
    } else if (parse_day(from, &t_from) != 0) {
        return finish_error(
            status, body, len, 400, "bad_request", "bad from date (use YYYY-MM-DD)");
    }
    if (to[0] == '\0') {
        time_t now = time(NULL);
        t_to = now - (now % 86400);
    } else if (parse_day(to, &t_to) != 0) {
        return finish_error(status, body, len, 400, "bad_request", "bad to date (use YYYY-MM-DD)");
    }

    int req_cap = USAGE_LIST_CAP;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    usage_row_t* rows = calloc((size_t)req_cap, sizeof *rows);
    if (rows == NULL) {
        return -1;
    }
    int n = 0;
    int rc = pg_store_ops(adm->ps)->query_usage(pg_store_ops(adm->ps)->ctx,
                                                key_id,
                                                model[0] != '\0' ? model : NULL,
                                                t_from,
                                                t_to,
                                                rows,
                                                req_cap,
                                                &n);
    if (rc != 0) {
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "usage query failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "key_id", json_integer(rows[i].key_id));
        json_object_set_new(o, "model_name", json_string(rows[i].model_name));
        json_object_set_new(o, "day", json_integer((int64_t)rows[i].day));
        json_object_set_new(o, "requests", json_integer(rows[i].requests));
        json_object_set_new(o, "prompt_tokens", json_integer(rows[i].prompt_tokens));
        json_object_set_new(o, "completion_tokens", json_integer(rows[i].completion_tokens));
        json_object_set_new(o, "cached_prompt_tokens", json_integer(rows[i].cached_prompt_tokens));
        json_object_set_new(o, "errors", json_integer(rows[i].errors));
        json_array_append_new(arr, o);
    }
    free(rows);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "key_id", json_integer(key_id));
    json_object_set_new(root, "usage", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief GET /admin/v1/usage/requests?key_id=&since=YYYY-MM-DD
 * Per-request audit detail (newest first); since empty = last 7 days. */
static int
usage_requests_query(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    char key[32] = "", since[16] = "";
    query_param(query, "key_id", key, sizeof key);
    query_param(query, "since", since, sizeof since);

    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    char* kend = NULL;
    long  key_id = strtol(key, &kend, 10);
    if (key[0] != '\0' && (kend == key || key_id <= 0)) {
        return finish_error(status, body, len, 400, "bad_request", "bad ?key_id=<id>");
    }
    time_t t_since;
    if (since[0] == '\0') {
        time_t now = time(NULL);
        t_since = now - (now % 86400) - 6 * 86400;
    } else if (parse_day(since, &t_since) != 0) {
        return finish_error(
            status, body, len, 400, "bad_request", "bad since date (use YYYY-MM-DD)");
    }

    int req_cap = USAGE_LIST_CAP;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    usage_request_row_t* rows = calloc((size_t)req_cap, sizeof *rows);
    if (rows == NULL) {
        return -1;
    }
    int n = 0;
    int rc = pg_store_ops(adm->ps)->query_usage_requests(
        pg_store_ops(adm->ps)->ctx, key_id, t_since, rows, req_cap, &n);
    if (rc != 0) {
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "request query failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t*   o = json_object();
        char      ts_iso[32];
        struct tm tmv;
        if (gmtime_r(&rows[i].ts, &tmv) != NULL) {
            strftime(ts_iso, sizeof ts_iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        } else {
            snprintf(ts_iso, sizeof ts_iso, "1970-01-01T00:00:00Z");
        }
        json_object_set_new(o, "key_id", json_integer(rows[i].key_id));
        json_object_set_new(o, "model", json_string(rows[i].model_name));
        json_object_set_new(o, "provider", json_string(rows[i].provider));
        json_object_set_new(o, "http_status", json_integer(rows[i].http_status));
        json_object_set_new(o, "prompt_tokens", json_integer(rows[i].prompt_tokens));
        json_object_set_new(o, "completion_tokens", json_integer(rows[i].completion_tokens));
        json_object_set_new(o, "cached_prompt_tokens", json_integer(rows[i].cached_prompt_tokens));
        json_object_set_new(o, "reasoning_tokens", json_integer(rows[i].reasoning_tokens));
        json_object_set_new(o, "latency_ms", json_real(rows[i].latency_ns / 1000000.0));
        json_object_set_new(o, "ts", json_string(ts_iso));
        json_object_set_new(o, "guardrail_action", json_string(rows[i].guardrail_action));
        json_array_append_new(arr, o);
    }
    free(rows);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "key_id", json_integer(key_id));
    json_object_set_new(root, "requests", arr);
    int dropped = adm->ac != NULL ? um_requests_dropped(adm->ac->um) : 0;
    json_object_set_new(root, "dropped", json_integer(dropped));
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/* ------------------------------------------------------------ groups */

static int
group_create(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    const char* name = jstring(jbody, "name", NULL);
    if (name == NULL || name[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "name is required");
    }
    if (strlen(name) > 64) {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "name too long (max 64 chars)");
    }
    double  monthly_budget_usd = 0.0;
    json_t* jmb = json_object_get(jbody, "monthly_budget_usd");
    if (jmb != NULL) {
        if (json_is_number(jmb)) {
            monthly_budget_usd = json_number_value(jmb);
        } else {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "monthly_budget_usd must be a number");
        }
    }
    char group_name[128];
    snprintf(group_name, sizeof group_name, "%s", name);
    long            id = 0;
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             rc = ops->create_group(ops->ctx, group_name, &id);
    json_decref(jbody);
    if (rc == -2) {
        return finish_error(status, body, len, 409, "group_exists", "group name already exists");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "group create failed");
    }
    if (monthly_budget_usd > 0.0) {
        ops->patch_group_budget(ops->ctx, id, monthly_budget_usd);
        if (adm->ac != NULL && adm->ac->be != NULL) {
            budget_enforce_set_group_budget(adm->ac->be, id, monthly_budget_usd);
        }
    }
    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    json_object_set_new(out, "name", json_string(group_name));
    json_object_set_new(out, "monthly_budget_usd", json_real(monthly_budget_usd));
    return finish_json(status, body, len, 201, out);
}

/** @brief GET /admin/v1/groups: list key groups with paging.
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
group_list(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    int req_cap = 256;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }
    group_rec_t* recs = calloc((size_t)req_cap, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_groups(ops->ctx, recs, req_cap, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "group list failed");
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "id", json_integer(recs[i].id));
        json_object_set_new(o, "name", json_string(recs[i].name));
        json_object_set_new(o, "key_count", json_integer(recs[i].key_count));
        json_object_set_new(o, "monthly_budget_usd", json_real(recs[i].monthly_budget_usd));
        if (recs[i].created_at > 0) {
            char      ts_iso[32];
            struct tm tmv;
            if (gmtime_r(&recs[i].created_at, &tmv) != NULL) {
                strftime(ts_iso, sizeof ts_iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            } else {
                snprintf(ts_iso, sizeof ts_iso, "1970-01-01T00:00:00Z");
            }
            json_object_set_new(o, "created_at", json_string(ts_iso));
        }
        json_array_append_new(arr, o);
    }
    free(recs);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "groups", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief PATCH/PUT /admin/v1/groups/<id>: rename a group / change its monthly budget by numeric id (at least one field required).
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
group_patch(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid group id");
    }
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }
    const char* name = jstring(jbody, "name", NULL);
    json_t*     jbudget = json_object_get(jbody, "monthly_budget_usd");
    if ((name == NULL || name[0] == '\0') && jbudget == NULL) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "name or monthly_budget_usd is required");
    }
    char            group_name[128] = {0};
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    if (name != NULL && name[0] != '\0') {
        if (strlen(name) > 64) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "name too long (max 64 chars)");
        }
        snprintf(group_name, sizeof group_name, "%s", name);
        int rc = ops->patch_group(ops->ctx, id, group_name);
        if (rc == -2) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 409, "group_exists", "group name already exists");
        }
        if (rc == 1) {
            json_decref(jbody);
            return finish_error(status, body, len, 404, "group_not_found", "group not found");
        }
        if (rc != 0) {
            json_decref(jbody);
            return finish_error(status, body, len, 500, "internal_error", "group patch failed");
        }
    }
    if (jbudget != NULL) {
        if (!json_is_number(jbudget)) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "monthly_budget_usd must be a number");
        }
        double budget = json_number_value(jbudget);
        if (budget < 0.0) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "monthly_budget_usd must be non-negative");
        }
        int rc = ops->patch_group_budget(ops->ctx, id, budget);
        if (rc == 1) {
            json_decref(jbody);
            return finish_error(status, body, len, 404, "group_not_found", "group not found");
        }
        if (rc != 0) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 500, "internal_error", "group budget patch failed");
        }
        if (adm->ac != NULL && adm->ac->be != NULL) {
            budget_enforce_set_group_budget(adm->ac->be, id, budget);
        }
    }
    json_decref(jbody);
    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    if (group_name[0] != '\0') {
        json_object_set_new(out, "name", json_string(group_name));
    }
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief DELETE /admin/v1/groups/<id>: delete a group; refused with 409 while keys are still attached.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
group_delete(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid group id");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    long            key_count = 0;
    if (ops->count_keys_in_group(ops->ctx, id, &key_count) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to check group members");
    }
    if (key_count > 0) {
        return finish_error(
            status, body, len, 409, "group_has_keys", "group still has attached api keys");
    }
    int rc = ops->delete_group(ops->ctx, id);
    if (rc == 1) {
        return finish_error(status, body, len, 404, "group_not_found", "group not found");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "group delete failed");
    }
    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    json_object_set_new(out, "deleted", json_true());
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ cost attribution */

/** @brief Look up a group name by group_id: 0 returns "(ungrouped)", unknown returns "(unknown)" (borrowed pointer, do not free). */
static const char*
lookup_group_name(long group_id, const group_rec_t* groups, int n_groups)
{
    if (group_id == 0) {
        return "(ungrouped)";
    }
    for (int i = 0; i < n_groups; i++) {
        if (groups[i].id == group_id) {
            return groups[i].name;
        }
    }
    return "(unknown)";
}

/** @brief Convert tokens into cents using model pricing (in/out_mtok unit prices, cache discount).
 *  @return 1 pricing hit with @p out_cents written; 0 on miss/illegal pricing (out untouched). */
static int
calculate_model_cost(const char*        model_name,
                     const model_rec_t* models,
                     int                n_models,
                     long               prompt,
                     long               completion,
                     long               cached,
                     long long*         out_cents)
{
    for (int i = 0; i < n_models; i++) {
        if (strcmp(models[i].name, model_name) == 0) {
            json_error_t jerr;
            json_t*      jp = json_loads(models[i].pricing_json, 0, &jerr);
            if (jp == NULL || !json_is_object(jp)) {
                if (jp != NULL) {
                    json_decref(jp);
                }
                return 0;
            }
            json_t* jin = json_object_get(jp, "in_mtok");
            json_t* jout = json_object_get(jp, "out_mtok");
            if (jin == NULL || jout == NULL || !json_is_number(jin) || !json_is_number(jout)) {
                json_decref(jp);
                return 0;
            }
            double  in_mtok = json_number_value(jin);
            double  out_mtok = json_number_value(jout);
            json_t* jdisc = json_object_get(jp, "cached_mtok_discount");
            double  cached_discount =
                (jdisc != NULL && json_is_number(jdisc)) ? json_number_value(jdisc) : 1.0;
            json_decref(jp);

            long   unhit_prompt = (prompt >= cached) ? (prompt - cached) : 0;
            double usd =
                ((double)unhit_prompt * in_mtok + (double)cached * in_mtok * cached_discount +
                 (double)completion * out_mtok) /
                1000000.0;
            *out_cents = (long long)llround(usd * 100.0);
            return 1;
        }
    }
    return 0;
}

/** @brief Per-model cost accumulator used while folding usage rows. */
struct model_agg {
    long group_id;        /**< owning group id (0 = ungrouped) */
    char group_name[128]; /**< resolved group display name */
    char model[128];      /**< model route name */
    long prompt;          /**< summed prompt tokens */
    long completion;      /**< summed completion tokens */
    long cached;          /**< summed cache-hit tokens */
    long requests;        /**< summed request count */
};

char*
cost_from_rows_paginated(const cost_row_t*  rows,
                         int                n_rows,
                         const model_rec_t* models,
                         int                n_models,
                         const group_rec_t* groups,
                         int                n_groups,
                         long               group_filter,
                         int                by_model,
                         int                truncated,
                         int                page,
                         int                limit)
{
    json_t* arr = json_array();
    if (arr == NULL) {
        return NULL;
    }

    if (by_model) {
        int               cap = n_rows > 0 ? n_rows : 1;
        struct model_agg* aggs = calloc((size_t)cap, sizeof *aggs);
        if (aggs == NULL) {
            json_decref(arr);
            return NULL;
        }
        int n_agg = 0;

        for (int i = 0; i < n_rows; i++) {
            if (group_filter >= 0 && rows[i].group_id != group_filter) {
                continue;
            }
            int idx = -1;
            for (int k = 0; k < n_agg; k++) {
                if (aggs[k].group_id == rows[i].group_id &&
                    strcmp(aggs[k].model, rows[i].model) == 0) {
                    idx = k;
                    break;
                }
            }
            if (idx >= 0) {
                aggs[idx].prompt += rows[i].prompt;
                aggs[idx].completion += rows[i].completion;
                aggs[idx].cached += rows[i].cached;
                aggs[idx].requests += rows[i].requests;
            } else if (n_agg < cap) {
                idx = n_agg++;
                aggs[idx].group_id = rows[i].group_id;
                snprintf(aggs[idx].group_name,
                         sizeof aggs[idx].group_name,
                         "%s",
                         lookup_group_name(rows[i].group_id, groups, n_groups));
                snprintf(aggs[idx].model, sizeof aggs[idx].model, "%s", rows[i].model);
                aggs[idx].prompt = rows[i].prompt;
                aggs[idx].completion = rows[i].completion;
                aggs[idx].cached = rows[i].cached;
                aggs[idx].requests = rows[i].requests;
            }
        }

        for (int k = 0; k < n_agg; k++) {
            json_t*   o = json_object();
            long long cost_cents = 0;
            int       has_cost = calculate_model_cost(aggs[k].model,
                                                      models,
                                                      n_models,
                                                      aggs[k].prompt,
                                                      aggs[k].completion,
                                                      aggs[k].cached,
                                                      &cost_cents);
            json_object_set_new(o, "group_id", json_integer(aggs[k].group_id));
            json_object_set_new(o, "group_name", json_string(aggs[k].group_name));
            json_object_set_new(o, "model", json_string(aggs[k].model));
            json_object_set_new(o, "prompt_tokens", json_integer(aggs[k].prompt));
            json_object_set_new(o, "completion_tokens", json_integer(aggs[k].completion));
            json_object_set_new(o, "cached_prompt_tokens", json_integer(aggs[k].cached));
            json_object_set_new(o, "requests", json_integer(aggs[k].requests));
            if (has_cost) {
                json_object_set_new(o, "cost_cents", json_integer(cost_cents));
            }
            json_array_append_new(arr, o);
        }
        free(aggs);
    } else {
        for (int i = 0; i < n_rows; i++) {
            if (group_filter >= 0 && rows[i].group_id != group_filter) {
                continue;
            }
            json_t*   o = json_object();
            char      day_str[32];
            struct tm tmv;
            if (gmtime_r(&rows[i].bucket_day, &tmv) != NULL) {
                strftime(day_str, sizeof day_str, "%Y-%m-%d", &tmv);
            } else {
                snprintf(day_str, sizeof day_str, "1970-01-01");
            }
            long long   cost_cents = 0;
            int         has_cost = calculate_model_cost(rows[i].model,
                                                        models,
                                                        n_models,
                                                        rows[i].prompt,
                                                        rows[i].completion,
                                                        rows[i].cached,
                                                        &cost_cents);
            const char* gname = lookup_group_name(rows[i].group_id, groups, n_groups);
            json_object_set_new(o, "date", json_string(day_str));
            json_object_set_new(o, "bucket_day", json_integer((int64_t)rows[i].bucket_day));
            json_object_set_new(o, "group_id", json_integer(rows[i].group_id));
            json_object_set_new(o, "group_name", json_string(gname));
            json_object_set_new(o, "model", json_string(rows[i].model));
            json_object_set_new(o, "prompt_tokens", json_integer(rows[i].prompt));
            json_object_set_new(o, "completion_tokens", json_integer(rows[i].completion));
            json_object_set_new(o, "cached_prompt_tokens", json_integer(rows[i].cached));
            json_object_set_new(o, "requests", json_integer(rows[i].requests));
            if (has_cost) {
                json_object_set_new(o, "cost_cents", json_integer(cost_cents));
            }
            json_array_append_new(arr, o);
        }
    }

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "rows", arr);
    json_object_set_new(root, "truncated", truncated ? json_true() : json_false());
    add_pagination_meta(root, total, page, limit);
    char* ret = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return ret;
}

/** @brief Pure calculation: transform cost_row_t rows + models pricing into a JSON cost report (unpaginated).
 *  Exported for unit testing. */
char*
cost_from_rows(const cost_row_t*  rows,
               int                n_rows,
               const model_rec_t* models,
               int                n_models,
               const group_rec_t* groups,
               int                n_groups,
               long               group_filter,
               int                by_model,
               int                truncated)
{
    return cost_from_rows_paginated(
        rows, n_rows, models, n_models, groups, n_groups, group_filter, by_model, truncated, 1, 0);
}

/** @brief GET /admin/v1/cost?group=&from=&to=&by=: cost attribution query (filter by group/date, group by the by field).
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
cost_query(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    char group_str[32] = "", from_str[32] = "", to_str[32] = "", by_str[32] = "";
    query_param(query, "group", group_str, sizeof group_str);
    query_param(query, "from", from_str, sizeof from_str);
    query_param(query, "to", to_str, sizeof to_str);
    query_param(query, "by", by_str, sizeof by_str);

    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    long group_filter = -1;
    if (group_str[0] != '\0') {
        char* end = NULL;
        group_filter = strtol(group_str, &end, 10);
        if (end == group_str || *end != '\0' || group_filter < 0) {
            return finish_error(status, body, len, 400, "bad_request", "invalid ?group=<id>");
        }
    }

    time_t now = time(NULL);
    time_t from_s = 0, to_s = 0;
    if (from_str[0] == '\0') {
        from_s = now - 30 * 86400;
    } else if (strchr(from_str, '-') != NULL) {
        if (parse_day(from_str, &from_s) != 0) {
            return finish_error(status, body, len, 400, "bad_request", "invalid from date");
        }
    } else {
        char* end = NULL;
        from_s = strtol(from_str, &end, 10);
        if (end == from_str || *end != '\0') {
            return finish_error(status, body, len, 400, "bad_request", "invalid from timestamp");
        }
    }

    if (to_str[0] == '\0') {
        to_s = now + 86400;
    } else if (strchr(to_str, '-') != NULL) {
        if (parse_day(to_str, &to_s) != 0) {
            return finish_error(status, body, len, 400, "bad_request", "invalid to date");
        }
        to_s += 86400;
    } else {
        char* end = NULL;
        to_s = strtol(to_str, &end, 10);
        if (end == to_str || *end != '\0') {
            return finish_error(status, body, len, 400, "bad_request", "invalid to timestamp");
        }
    }

    if (from_s < 0 || to_s < 0 || from_s >= to_s) {
        return finish_error(status, body, len, 400, "bad_request", "from must be before to");
    }
    if ((to_s - from_s) > 366 * 86400) {
        return finish_error(status, body, len, 400, "bad_request", "time range exceeds 365 days");
    }

    int by_model = (strcmp(by_str, "model") == 0);

    const pg_ops_t* ops = pg_store_ops(adm->ps);
    cost_row_t*     rows = calloc(4096, sizeof *rows);
    if (rows == NULL) {
        return -1;
    }
    int n_rows = 0;
    if (ops->query_cost(ops->ctx, (long)from_s, (long)to_s, rows, 4096, &n_rows) != 0) {
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "cost query failed");
    }
    int truncated = (n_rows >= 4096);

    model_rec_t* models = calloc(MODEL_LIST_CAP, sizeof *models);
    if (models == NULL) {
        free(rows);
        return -1;
    }
    int n_models = 0;
    if (ops->list_models(ops->ctx, models, MODEL_LIST_CAP, &n_models) != 0) {
        free(models);
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "failed to list models");
    }

    group_rec_t groups[256];
    int         n_groups = 0;
    if (ops->list_groups(ops->ctx, groups, 256, &n_groups) != 0) {
        for (int i = 0; i < n_models; i++) {
            model_rec_free(&models[i]);
        }
        free(models);
        free(rows);
        return finish_error(status, body, len, 500, "internal_error", "failed to list groups");
    }

    char* json_str = cost_from_rows_paginated(rows,
                                              n_rows,
                                              models,
                                              n_models,
                                              groups,
                                              n_groups,
                                              group_filter,
                                              by_model,
                                              truncated,
                                              page,
                                              limit);

    for (int i = 0; i < n_models; i++) {
        model_rec_free(&models[i]);
    }
    free(models);
    free(rows);

    if (json_str == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "cost formatting failed");
    }

    *status = 200;
    *body = json_str;
    *len = strlen(json_str);
    return 0;
}

/* ------------------------------------------------------------ guardrails */

static int
guardrails_rule_create(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* pattern = jstring(jbody, "pattern", NULL);
    if (pattern == NULL || pattern[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "pattern is required");
    }
    if (strlen(pattern) >= 512) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "pattern too long (max 511 chars)");
    }

    const char* rule_type = jstring(jbody, "rule_type", "keyword");
    if (strcmp(rule_type, "keyword") != 0 && strcmp(rule_type, "regex") != 0 &&
        strcmp(rule_type, "pii") != 0 && strcmp(rule_type, "webhook") != 0) {
        json_decref(jbody);
        return finish_error(status,
                            body,
                            len,
                            400,
                            "bad_request",
                            "rule_type must be keyword, regex, pii, or webhook");
    }

    if (strcmp(rule_type, "webhook") == 0) {
        if (strncmp(pattern, "http://", 7) != 0 && strncmp(pattern, "https://", 8) != 0) {
            json_decref(jbody);
            return finish_error(status,
                                body,
                                len,
                                400,
                                "bad_request",
                                "webhook pattern must be http:// or https:// URL");
        }
    }

    const char* action = jstring(jbody, "action", "block");
    if (strcmp(action, "block") != 0 && strcmp(action, "mask") != 0) {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "action must be block or mask");
    }

    const char* category = jstring(jbody, "category", "general");
    if (strlen(category) >= 64) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "category too long (max 63 chars)");
    }

    const char* webhook_secret = jstring(jbody, "webhook_secret", "");
    int         timeout_ms = 500;
    json_t*     jt = json_object_get(jbody, "timeout_ms");
    if (jt != NULL && json_is_integer(jt)) {
        timeout_ms = (int)json_integer_value(jt);
    }
    if (timeout_ms <= 0) {
        timeout_ms = 500;
    }

    const char* fail_mode = jstring(jbody, "fail_mode", "open");
    if (strcmp(fail_mode, "open") != 0 && strcmp(fail_mode, "closed") != 0) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "fail_mode must be open or closed");
    }

    const char* phase = jstring(jbody, "phase", "inbound");
    if (strcmp(phase, "inbound") != 0 && strcmp(phase, "outbound") != 0 &&
        strcmp(phase, "both") != 0) {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "phase must be inbound, outbound, or both");
    }

    int     enabled = 1;
    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL) {
        if (json_is_boolean(jen)) {
            enabled = json_is_true(jen) ? 1 : 0;
        } else if (json_is_integer(jen)) {
            enabled = json_integer_value(jen) ? 1 : 0;
        }
    }

    guardrail_rule_t rule;
    memset(&rule, 0, sizeof rule);
    snprintf(rule.rule_type, sizeof rule.rule_type, "%s", rule_type);
    snprintf(rule.pattern, sizeof rule.pattern, "%s", pattern);
    snprintf(rule.action, sizeof rule.action, "%s", action);
    snprintf(rule.category, sizeof rule.category, "%s", category);
    rule.enabled = enabled;
    snprintf(rule.webhook_secret, sizeof rule.webhook_secret, "%s", webhook_secret);
    rule.timeout_ms = timeout_ms;
    snprintf(rule.fail_mode, sizeof rule.fail_mode, "%s", fail_mode);
    snprintf(rule.phase, sizeof rule.phase, "%s", phase);

    const pg_ops_t* ops = pg_store_ops(adm->ps);
    long            new_id = 0;
    int             rc = ops->create_guardrails_rule(ops->ctx, &rule, &new_id);
    json_decref(jbody);
    if (rc != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "guardrails rule create failed");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_guardrails(adm->ac);
    }

    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(new_id));
    json_object_set_new(out, "rule_type", json_string(rule.rule_type));
    json_object_set_new(out, "pattern", json_string(rule.pattern));
    json_object_set_new(out, "action", json_string(rule.action));
    json_object_set_new(out, "category", json_string(rule.category));
    json_object_set_new(out, "enabled", json_boolean(rule.enabled));
    json_object_set_new(out, "webhook_secret", json_string(rule.webhook_secret));
    json_object_set_new(out, "timeout_ms", json_integer(rule.timeout_ms));
    json_object_set_new(out, "fail_mode", json_string(rule.fail_mode));
    json_object_set_new(out, "phase", json_string(rule.phase));
    return finish_json(status, body, len, 201, out);
}

/** @brief GET /admin/v1/guardrails: list guardrail rules with paging.
 *  @return 0 with status/body filled; -1 only on memory/serialization failure. */
static int
guardrails_rule_list(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    int page = 1, limit = 0;
    parse_pagination_params(query, &page, &limit);

    int req_cap = 256;
    if (limit > 0 && page > 0) {
        int needed = page * limit;
        if (needed > req_cap) {
            req_cap = needed <= 4096 ? needed : 4096;
        }
    }

    guardrail_rule_t* recs = calloc((size_t)req_cap, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_guardrails_rules(ops->ctx, recs, req_cap, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "guardrails list failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "id", json_integer(recs[i].id));
        json_object_set_new(o, "rule_type", json_string(recs[i].rule_type));
        json_object_set_new(o, "pattern", json_string(recs[i].pattern));
        json_object_set_new(o, "action", json_string(recs[i].action));
        json_object_set_new(o, "category", json_string(recs[i].category));
        json_object_set_new(o, "enabled", json_boolean(recs[i].enabled));
        json_object_set_new(o, "webhook_secret", json_string(recs[i].webhook_secret));
        json_object_set_new(o, "timeout_ms", json_integer(recs[i].timeout_ms));
        json_object_set_new(o, "fail_mode", json_string(recs[i].fail_mode));
        json_object_set_new(o, "phase", json_string(recs[i].phase));
        if (recs[i].created_at > 0) {
            char      ts_iso[32];
            struct tm tmv;
            if (gmtime_r(&recs[i].created_at, &tmv) != NULL) {
                strftime(ts_iso, sizeof ts_iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            } else {
                snprintf(ts_iso, sizeof ts_iso, "1970-01-01T00:00:00Z");
            }
            json_object_set_new(o, "created_at", json_string(ts_iso));
        }
        json_array_append_new(arr, o);
    }
    free(recs);

    size_t total = 0;
    arr = paginate_json_array(arr, page, limit, &total);

    json_t* root = json_object();
    json_object_set_new(root, "rules", arr);
    add_pagination_meta(root, total, page, limit);
    return finish_json(status, body, len, 200, root);
}

/** @brief PATCH/PUT /admin/v1/guardrails/<id>: update a guardrail rule by numeric id.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
guardrails_rule_update(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }

    const pg_ops_t*  ops = pg_store_ops(adm->ps);
    guardrail_rule_t existing;
    memset(&existing, 0, sizeof existing);
    int              found = 0;
    guardrail_rule_t buf[256];
    int              n = 0;
    if (ops->list_guardrails_rules(ops->ctx, buf, 256, &n) == 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i].id == id) {
                existing = buf[i];
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        return finish_error(status, body, len, 404, "rule_not_found", "rule not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* rt = jstring(jbody, "rule_type", NULL);
    if (rt != NULL) {
        if (strcmp(rt, "keyword") != 0 && strcmp(rt, "regex") != 0 && strcmp(rt, "pii") != 0 &&
            strcmp(rt, "webhook") != 0) {
            json_decref(jbody);
            return finish_error(status,
                                body,
                                len,
                                400,
                                "bad_request",
                                "rule_type must be keyword, regex, pii, or webhook");
        }
        snprintf(existing.rule_type, sizeof existing.rule_type, "%s", rt);
    }

    const char* pat = jstring(jbody, "pattern", NULL);
    if (pat != NULL) {
        if (pat[0] == '\0' || strlen(pat) >= 512) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "pattern invalid (1-511 chars)");
        }
        if (strcmp(existing.rule_type, "webhook") == 0) {
            if (strncmp(pat, "http://", 7) != 0 && strncmp(pat, "https://", 8) != 0) {
                json_decref(jbody);
                return finish_error(status,
                                    body,
                                    len,
                                    400,
                                    "bad_request",
                                    "webhook pattern must be http:// or https:// URL");
            }
        }
        snprintf(existing.pattern, sizeof existing.pattern, "%s", pat);
    }

    const char* act = jstring(jbody, "action", NULL);
    if (act != NULL) {
        if (strcmp(act, "block") != 0 && strcmp(act, "mask") != 0) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "action must be block or mask");
        }
        snprintf(existing.action, sizeof existing.action, "%s", act);
    }

    const char* cat = jstring(jbody, "category", NULL);
    if (cat != NULL) {
        if (strlen(cat) >= 64) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "category too long (max 63 chars)");
        }
        snprintf(existing.category, sizeof existing.category, "%s", cat);
    }

    const char* ws = jstring(jbody, "webhook_secret", NULL);
    if (ws != NULL) {
        snprintf(existing.webhook_secret, sizeof existing.webhook_secret, "%s", ws);
    }

    json_t* jt = json_object_get(jbody, "timeout_ms");
    if (jt != NULL && json_is_integer(jt)) {
        int to = (int)json_integer_value(jt);
        existing.timeout_ms = to > 0 ? to : 500;
    }

    const char* fm = jstring(jbody, "fail_mode", NULL);
    if (fm != NULL) {
        if (strcmp(fm, "open") != 0 && strcmp(fm, "closed") != 0) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "fail_mode must be open or closed");
        }
        snprintf(existing.fail_mode, sizeof existing.fail_mode, "%s", fm);
    }

    const char* ph = jstring(jbody, "phase", NULL);
    if (ph != NULL) {
        if (strcmp(ph, "inbound") != 0 && strcmp(ph, "outbound") != 0 && strcmp(ph, "both") != 0) {
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "phase must be inbound, outbound, or both");
        }
        snprintf(existing.phase, sizeof existing.phase, "%s", ph);
    }

    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL) {
        if (json_is_boolean(jen)) {
            existing.enabled = json_is_true(jen) ? 1 : 0;
        } else if (json_is_integer(jen)) {
            existing.enabled = json_integer_value(jen) ? 1 : 0;
        }
    }
    json_decref(jbody);

    int rc = ops->update_guardrails_rule(ops->ctx, &existing);
    if (rc == 1) {
        return finish_error(status, body, len, 404, "rule_not_found", "rule not found");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "rule update failed");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_guardrails(adm->ac);
    }

    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    json_object_set_new(out, "rule_type", json_string(existing.rule_type));
    json_object_set_new(out, "pattern", json_string(existing.pattern));
    json_object_set_new(out, "action", json_string(existing.action));
    json_object_set_new(out, "category", json_string(existing.category));
    json_object_set_new(out, "enabled", json_boolean(existing.enabled));
    json_object_set_new(out, "webhook_secret", json_string(existing.webhook_secret));
    json_object_set_new(out, "timeout_ms", json_integer(existing.timeout_ms));
    json_object_set_new(out, "fail_mode", json_string(existing.fail_mode));
    json_object_set_new(out, "phase", json_string(existing.phase));
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief DELETE /admin/v1/guardrails/<id>: delete a rule and hot-reload pipeline guardrails.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
guardrails_rule_delete(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             rc = ops->delete_guardrails_rule(ops->ctx, id);
    if (rc == 1) {
        return finish_error(status, body, len, 404, "rule_not_found", "rule not found");
    }
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "rule delete failed");
    }
    if (adm->ac != NULL) {
        aigate_core_reload_guardrails(adm->ac);
    }
    json_t* out = json_object();
    json_object_set_new(out, "id", json_integer(id));
    json_object_set_new(out, "deleted", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief POST /admin/v1/guardrails/reload: reload pipeline guardrail rules from the store.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
guardrails_reload(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    if (adm->ac != NULL) {
        aigate_core_reload_guardrails(adm->ac);
    }
    json_t* out = json_object();
    json_object_set_new(out, "status", json_string("reloaded"));
    return finish_json(status, body, len, 200, out);
}

/** @brief POST /admin/v1/guardrails/webhook/test: probe external webhook connectivity and latency.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
guardrails_webhook_test_handler(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    (void)adm;
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* url = jstring(jbody, "url", NULL);
    if (url == NULL || url[0] == '\0') {
        url = jstring(jbody, "pattern", NULL);
    }
    if (url == NULL || url[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "url is required");
    }

    const char* secret = jstring(jbody, "webhook_secret", "");
    if (secret[0] == '\0') {
        secret = jstring(jbody, "secret", "");
    }

    int     timeout_ms = 1000;
    json_t* jt = json_object_get(jbody, "timeout_ms");
    if (jt != NULL && json_is_integer(jt)) {
        timeout_ms = (int)json_integer_value(jt);
    }
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }

    char   err_msg[256] = {0};
    double latency_ms = 0.0;
    int    prc =
        guardrails_webhook_probe(url, secret, timeout_ms, err_msg, sizeof err_msg, &latency_ms);
    json_decref(jbody);

    json_t* out = json_object();
    if (prc == 0) {
        json_object_set_new(out, "status", json_string("ok"));
        json_object_set_new(out, "latency_ms", json_real(latency_ms));
        json_object_set_new(out, "reachable", json_true());
    } else {
        json_object_set_new(out, "status", json_string("error"));
        json_object_set_new(out, "error", json_string(err_msg[0] ? err_msg : "probe failed"));
        json_object_set_new(out, "reachable", json_false());
    }
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ PII guardrails */

static const char*
pii_type_to_str(pii_type_t t)
{
    switch (t) {
    case PII_TYPE_PHONE:
        return "phone";
    case PII_TYPE_ID_CARD:
        return "id_card";
    case PII_TYPE_BANK_CARD:
        return "bank_card";
    case PII_TYPE_EMAIL:
        return "email";
    case PII_TYPE_API_KEY:
        return "api_key";
    case PII_TYPE_IP_ADDRESS:
        return "ip_address";
    default:
        return "unknown";
    }
}

static pii_type_t
pii_type_from_str(const char* s)
{
    if (s == NULL) {
        return PII_TYPE_COUNT;
    }
    if (strcmp(s, "phone") == 0) {
        return PII_TYPE_PHONE;
    }
    if (strcmp(s, "id_card") == 0) {
        return PII_TYPE_ID_CARD;
    }
    if (strcmp(s, "bank_card") == 0) {
        return PII_TYPE_BANK_CARD;
    }
    if (strcmp(s, "email") == 0) {
        return PII_TYPE_EMAIL;
    }
    if (strcmp(s, "api_key") == 0) {
        return PII_TYPE_API_KEY;
    }
    if (strcmp(s, "ip_address") == 0 || strcmp(s, "ip") == 0) {
        return PII_TYPE_IP_ADDRESS;
    }
    return PII_TYPE_COUNT;
}

static const char*
pii_action_to_str(pii_action_t act)
{
    switch (act) {
    case PII_ACTION_OFF:
        return "off";
    case PII_ACTION_ANONYMIZE_RESTORE:
        return "anonymize_restore";
    case PII_ACTION_MASK_PARTIAL:
        return "mask_partial";
    case PII_ACTION_REDACT_TAG:
        return "redact_tag";
    case PII_ACTION_BLOCK:
        return "block";
    default:
        return "off";
    }
}

static pii_action_t
pii_action_from_str(const char* s)
{
    if (s == NULL) {
        return PII_ACTION_OFF;
    }
    if (strcmp(s, "anonymize_restore") == 0) {
        return PII_ACTION_ANONYMIZE_RESTORE;
    }
    if (strcmp(s, "mask_partial") == 0) {
        return PII_ACTION_MASK_PARTIAL;
    }
    if (strcmp(s, "redact_tag") == 0) {
        return PII_ACTION_REDACT_TAG;
    }
    if (strcmp(s, "block") == 0) {
        return PII_ACTION_BLOCK;
    }
    return PII_ACTION_OFF;
}

/** @brief GET /admin/v1/guardrails/pii: get active PII rules configuration. */
static int
guardrails_pii_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    guardrails_ctx_t* gr = (adm != NULL && adm->ac != NULL) ? adm->ac->gr : NULL;
    pii_config_t      cfg;
    if (gr != NULL) {
        cfg = guardrails_get_pii_config(gr);
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }

    json_t* arr = json_array();
    for (int i = 0; i < PII_TYPE_COUNT; i++) {
        json_t* item = json_object();
        json_object_set_new(item, "entity", json_string(pii_type_to_str((pii_type_t)i)));
        json_object_set_new(
            item,
            "name",
            json_string(cfg.rules[i].name[0] ? cfg.rules[i].name : pii_type_to_str((pii_type_t)i)));
        json_object_set_new(item, "tag", json_string(cfg.rules[i].tag));
        json_object_set_new(item, "enabled", json_boolean(cfg.rules[i].enabled));
        json_object_set_new(item, "action", json_string(pii_action_to_str(cfg.rules[i].action)));
        json_array_append_new(arr, item);
    }

    json_t* out = json_object();
    json_object_set_new(out, "rules", arr);
    return finish_json(status, body, len, 200, out);
}

/** @brief PUT /admin/v1/guardrails/pii: update PII rules configuration. */
static int
guardrails_pii_put(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    if (req_body == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "missing request body");
    }
    json_error_t jerr;
    json_t*      root = json_loads((const char*)req_body, 0, &jerr);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return finish_error(status, body, len, 400, "bad_request", "invalid json object");
    }

    guardrails_ctx_t* gr = (adm != NULL && adm->ac != NULL) ? adm->ac->gr : NULL;
    if (gr == NULL) {
        json_decref(root);
        return finish_error(
            status, body, len, 500, "internal_error", "guardrails engine not initialized");
    }

    pii_config_t cfg = guardrails_get_pii_config(gr);

    json_t* arr = json_object_get(root, "rules");
    if (arr != NULL && json_is_array(arr)) {
        size_t  idx;
        json_t* item;
        json_array_foreach(arr, idx, item)
        {
            if (!json_is_object(item)) {
                continue;
            }
            const char* ent_str = json_string_value(json_object_get(item, "entity"));
            if (ent_str == NULL) {
                ent_str = json_string_value(json_object_get(item, "name"));
            }
            pii_type_t t = pii_type_from_str(ent_str);
            if (t >= PII_TYPE_COUNT) {
                continue;
            }
            json_t* en = json_object_get(item, "enabled");
            if (en != NULL && json_is_boolean(en)) {
                cfg.rules[t].enabled = json_is_true(en);
            }
            const char* act_str = json_string_value(json_object_get(item, "action"));
            if (act_str != NULL) {
                cfg.rules[t].action = pii_action_from_str(act_str);
            }
        }
    }

    guardrails_set_pii_config(gr, &cfg);
    json_decref(root);

    json_t* out = json_object();
    json_object_set_new(out, "status", json_string("ok"));
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

/** @brief POST /admin/v1/guardrails/pii/test: simulate PII anonymization and restoration in real-time. */
static int
guardrails_pii_test(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    if (req_body == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "missing request body");
    }
    json_error_t jerr;
    json_t*      root = json_loads((const char*)req_body, 0, &jerr);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return finish_error(status, body, len, 400, "bad_request", "invalid json object");
    }

    const char* text = json_string_value(json_object_get(root, "text"));
    if (text == NULL) {
        json_decref(root);
        return finish_error(status, body, len, 400, "bad_request", "missing 'text' field");
    }

    guardrails_ctx_t* gr = (adm != NULL && adm->ac != NULL) ? adm->ac->gr : NULL;
    if (gr == NULL) {
        json_decref(root);
        return finish_error(
            status, body, len, 500, "internal_error", "guardrails engine not initialized");
    }

    pii_session_map_t map;
    memset(&map, 0, sizeof(map));
    pii_action_t max_action = PII_ACTION_OFF;
    int          changed = 0;

    char* trans =
        guardrails_transform_pii_text(gr, text, strlen(text), &map, &max_action, &changed);
    const char* anon_text = (changed && trans != NULL) ? trans : text;

    int   r_changed = 0;
    char* restored = guardrails_restore_pii_text(anon_text, strlen(anon_text), &map, &r_changed);
    const char* rest_text = (r_changed && restored != NULL) ? restored : anon_text;

    json_t* det_arr = json_array();
    for (int i = 0; i < map.count; i++) {
        json_t* det = json_object();
        json_object_set_new(det, "type", json_string(pii_type_to_str(map.entries[i].type)));
        json_object_set_new(det, "placeholder", json_string(map.entries[i].placeholder));
        json_object_set_new(det, "original", json_string(map.entries[i].original));
        json_array_append_new(det_arr, det);
    }

    json_t* out = json_object();
    json_object_set_new(out, "anonymized", json_string(anon_text));
    json_object_set_new(out, "restored_preview", json_string(rest_text));
    json_object_set_new(out, "detected_entities", det_arr);
    json_object_set_new(out, "highest_action", json_string(pii_action_to_str(max_action)));

    free(trans);
    free(restored);
    json_decref(root);
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ response cache */

static int
cache_stats_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    if (adm->rc == NULL) {
        return finish_error(
            status, body, len, 503, "unavailable", "response cache not initialized");
    }
    char* json_str = response_cache_get_stats_json(adm->rc);
    if (json_str == NULL) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to serialize cache stats");
    }
    *status = 200;
    *body = json_str;
    *len = strlen(json_str);
    return 0;
}

/** @brief POST /admin/v1/cache/purge: purge the response cache by request-body model (default purges all), returning entry/byte counts.
 *  @return 0 with status/body filled; -1 only on JSON serialization failure. */
static int
cache_purge_trigger(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* req_body)
{
    if (adm->rc == NULL) {
        return finish_error(
            status, body, len, 503, "unavailable", "response cache not initialized");
    }
    const char* model = NULL;
    json_t*     root = NULL;
    if (req_body != NULL && req_body[0] != '\0') {
        json_error_t err;
        root = json_loads(req_body, 0, &err);
        if (root != NULL && json_is_object(root)) {
            json_t* j_m = json_object_get(root, "model");
            if (json_is_string(j_m)) {
                model = json_string_value(j_m);
            }
        }
    }

    size_t purged_count = 0;
    size_t freed_bytes = 0;
    response_cache_purge(adm->rc, model, &purged_count, &freed_bytes);

    json_t* out = json_object();
    json_object_set_new(out, "purged_entries", json_integer((json_int_t)purged_count));
    json_object_set_new(out, "freed_bytes", json_integer((json_int_t)freed_bytes));
    json_object_set_new(out, "model", json_string(model != NULL ? model : "all"));

    if (root != NULL) {
        json_decref(root);
    }
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ distributed tracing */

/**
 * @brief GET /admin/v1/traces/config: retrieve current tracing configuration and buffer stats.
 *
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on fatal allocation error.
 */
static int
admin_traces_config_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    tracer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    size_t   buffered = 0;
    uint64_t dropped = 0;

    if (adm != NULL && adm->ac != NULL) {
        if (adm->ac->tm != NULL) {
            cfg = tracer_manager_get_config(adm->ac->tm);
        } else {
            cfg = adm->ac->tracer_cfg;
        }
        if (adm->ac->trace_rb != NULL) {
            buffered = trace_ring_buffer_count(adm->ac->trace_rb);
            dropped = trace_ring_buffer_dropped(adm->ac->trace_rb);
        }
    }

    json_t* out = json_object();
    json_object_set_new(out, "enabled", json_boolean(cfg.enabled));
    json_object_set_new(out, "sample_rate", json_real(cfg.sample_rate));
    json_object_set_new(out, "slow_threshold_ms", json_integer((json_int_t)cfg.slow_threshold_ms));
    json_object_set_new(out, "otlp_endpoint", json_string(cfg.otlp_endpoint));
    json_object_set_new(out, "buffered_count", json_integer((json_int_t)buffered));
    json_object_set_new(out, "dropped_count", json_integer((json_int_t)dropped));

    return finish_json(status, body, len, 200, out);
}

/**
 * @brief PUT /admin/v1/traces/config: dynamically update tracing configuration.
 *
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param req_body JSON payload containing configuration options.
 * @return 0 on success, negative on fatal allocation error.
 */
static int
admin_traces_config_put(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* req_body)
{
    if (req_body == NULL || req_body[0] == '\0') {
        return finish_error(status, body, len, 400, "invalid_request", "empty request body");
    }

    json_error_t err;
    json_t*      root = json_loads(req_body, 0, &err);
    if (root == NULL || !json_is_object(root)) {
        return finish_error(status, body, len, 400, "invalid_json", "failed to parse json payload");
    }

    if (adm == NULL || adm->ac == NULL) {
        json_decref(root);
        return finish_error(status, body, len, 503, "unavailable", "core not initialized");
    }

    tracer_config_t cfg;
    if (adm->ac->tm != NULL) {
        cfg = tracer_manager_get_config(adm->ac->tm);
    } else {
        cfg = adm->ac->tracer_cfg;
    }

    json_t* j_en = json_object_get(root, "enabled");
    if (j_en != NULL && json_is_boolean(j_en)) {
        cfg.enabled = json_is_true(j_en);
    }

    json_t* j_sr = json_object_get(root, "sample_rate");
    if (j_sr != NULL && json_is_number(j_sr)) {
        double sr = json_number_value(j_sr);
        if (sr < 0.0) {
            sr = 0.0;
        }
        if (sr > 1.0) {
            sr = 1.0;
        }
        cfg.sample_rate = sr;
    }

    json_t* j_st = json_object_get(root, "slow_threshold_ms");
    if (j_st != NULL && json_is_integer(j_st)) {
        json_int_t st = json_integer_value(j_st);
        if (st >= 0) {
            cfg.slow_threshold_ms = (uint32_t)st;
        }
    }

    json_t* j_ep = json_object_get(root, "otlp_endpoint");
    if (j_ep != NULL && json_is_string(j_ep)) {
        const char* ep = json_string_value(j_ep);
        strncpy(cfg.otlp_endpoint, ep, sizeof(cfg.otlp_endpoint) - 1);
        cfg.otlp_endpoint[sizeof(cfg.otlp_endpoint) - 1] = '\0';
    }

    json_decref(root);

    adm->ac->tracer_cfg = cfg;
    if (adm->ac->tm != NULL) {
        tracer_manager_update_config(adm->ac->tm, &cfg);
    }

    return admin_traces_config_get(adm, status, body, len);
}

/**
 * @brief GET /admin/v1/traces/:trace_id: retrieve full trace timeline, spans, and attributes.
 *
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param trace_id 32-hex trace ID to look up.
 * @return 0 on success, negative on fatal allocation error.
 */
static int
admin_trace_get_by_id(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* trace_id)
{
    (void)adm;
    if (trace_id == NULL || trace_id[0] == '\0') {
        return finish_error(
            status, body, len, 400, "invalid_trace_id", "trace id must not be empty");
    }

    trace_context_t ctx;
    if (!tracer_cache_get(trace_id, &ctx)) {
        return finish_error(
            status, body, len, 404, "trace_not_found", "trace id not found in recent traces cache");
    }

    json_t* out = json_object();
    json_object_set_new(out, "trace_id", json_string(ctx.trace_id));
    json_object_set_new(out, "root_span_id", json_string(ctx.root_span_id));
    json_object_set_new(out, "is_sampled", json_boolean(ctx.is_sampled));

    uint64_t root_start_ns = 0;
    uint64_t root_end_ns = 0;
    for (int i = 0; i < ctx.span_count; i++) {
        if (strcmp(ctx.spans[i].name, "root") == 0) {
            root_start_ns = ctx.spans[i].start_time_ns;
            root_end_ns = ctx.spans[i].end_time_ns;
            break;
        }
    }
    if (root_start_ns == 0 && ctx.span_count > 0) {
        root_start_ns = ctx.spans[0].start_time_ns;
        root_end_ns = ctx.spans[0].end_time_ns;
    }

    double total_duration_ms = 0.0;
    if (root_end_ns >= root_start_ns && root_start_ns > 0) {
        total_duration_ms = (double)(root_end_ns - root_start_ns) / 1e6;
    }
    json_object_set_new(out, "total_duration_ms", json_real(total_duration_ms));

    json_t* j_spans = json_array();
    for (int i = 0; i < ctx.span_count; i++) {
        const trace_span_t* sp = &ctx.spans[i];
        json_t*             j_sp = json_object();
        json_object_set_new(j_sp, "span_id", json_string(sp->span_id));
        json_object_set_new(j_sp, "parent_span_id", json_string(sp->parent_span_id));
        json_object_set_new(j_sp, "name", json_string(sp->name));

        const char* kind_str = "internal";
        if (sp->kind == SPAN_KIND_SERVER) {
            kind_str = "server";
        } else if (sp->kind == SPAN_KIND_CLIENT) {
            kind_str = "client";
        }
        json_object_set_new(j_sp, "kind", json_string(kind_str));

        double offset_ms = 0.0;
        if (sp->start_time_ns >= root_start_ns && root_start_ns > 0) {
            offset_ms = (double)(sp->start_time_ns - root_start_ns) / 1e6;
        }
        json_object_set_new(j_sp, "start_offset_ms", json_real(offset_ms));

        double dur_ms = 0.0;
        if (sp->end_time_ns >= sp->start_time_ns && sp->start_time_ns > 0) {
            dur_ms = (double)(sp->end_time_ns - sp->start_time_ns) / 1e6;
        }
        json_object_set_new(j_sp, "duration_ms", json_real(dur_ms));

        const char* status_str = "unset";
        if (sp->status == SPAN_STATUS_OK) {
            status_str = "ok";
        } else if (sp->status == SPAN_STATUS_ERROR) {
            status_str = "error";
        }
        json_object_set_new(j_sp, "status", json_string(status_str));

        if (sp->status_desc[0] != '\0') {
            json_object_set_new(j_sp, "status_desc", json_string(sp->status_desc));
        }

        json_t* attrs = json_object();
        for (int a = 0; a < sp->attr_count; a++) {
            const char* v = sp->attributes[a].value;
            if (strcmp(v, "true") == 0) {
                json_object_set_new(attrs, sp->attributes[a].key, json_true());
            } else if (strcmp(v, "false") == 0) {
                json_object_set_new(attrs, sp->attributes[a].key, json_false());
            } else {
                char*     endptr = NULL;
                long long iv = strtoll(v, &endptr, 10);
                if (*endptr == '\0' && endptr != v) {
                    json_object_set_new(attrs, sp->attributes[a].key, json_integer((json_int_t)iv));
                } else {
                    double dv = strtod(v, &endptr);
                    if (*endptr == '\0' && endptr != v && strchr(v, '.') != NULL) {
                        json_object_set_new(attrs, sp->attributes[a].key, json_real(dv));
                    } else {
                        json_object_set_new(attrs, sp->attributes[a].key, json_string(v));
                    }
                }
            }
        }
        json_object_set_new(j_sp, "attributes", attrs);
        json_array_append_new(j_spans, j_sp);
    }
    json_object_set_new(out, "spans", j_spans);

    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/traces: list recently recorded traces.
 *
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on fatal allocation error.
 */
static int
admin_traces_list_recent(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    (void)adm;
    trace_context_t list[100];
    size_t          count = tracer_cache_list_recent(list, 100);

    json_t* arr = json_array();
    for (size_t i = 0; i < count; i++) {
        const trace_context_t* ctx = &list[i];
        json_t*                item = json_object();
        json_object_set_new(item, "trace_id", json_string(ctx->trace_id));
        json_object_set_new(item, "root_span_id", json_string(ctx->root_span_id));
        json_object_set_new(item, "is_sampled", json_boolean(ctx->is_sampled));
        json_object_set_new(item, "span_count", json_integer(ctx->span_count));

        uint64_t root_start_ns = 0;
        uint64_t root_end_ns = 0;
        for (int s = 0; s < ctx->span_count; s++) {
            if (strcmp(ctx->spans[s].name, "root") == 0) {
                root_start_ns = ctx->spans[s].start_time_ns;
                root_end_ns = ctx->spans[s].end_time_ns;
                break;
            }
        }
        double total_ms = 0.0;
        if (root_end_ns >= root_start_ns && root_start_ns > 0) {
            total_ms = (double)(root_end_ns - root_start_ns) / 1e6;
        }
        json_object_set_new(item, "total_duration_ms", json_real(total_ms));
        json_object_set_new(
            item, "timestamp_us", json_integer((json_int_t)ctx->req_start_realtime_us));
        json_array_append_new(arr, item);
    }

    json_t* out = json_object();
    json_object_set_new(out, "traces", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief Convert shadow_rule_t to a JSON object.
 * @param r Pointer to shadow rule.
 * @return Newly allocated JSON object.
 */
static json_t*
shadow_rule_to_json(const shadow_rule_t* r)
{
    json_t* o = json_object();
    json_object_set_new(o, "id", json_integer((json_int_t)r->id));
    json_object_set_new(o, "source_model", json_string(r->source_model));
    json_object_set_new(o, "target_model", json_string(r->target_model));
    json_object_set_new(o, "target_provider", json_string(r->target_provider));
    json_object_set_new(
        o, "mode", json_string(r->mode == TRAFFIC_MODE_CANARY ? "CANARY" : "SHADOW"));
    json_object_set_new(o, "sample_rate", json_real(r->sample_rate));
    json_object_set_new(o, "header_match", json_string(r->header_match));
    json_object_set_new(o, "enabled", json_boolean(r->enabled));
    json_object_set_new(o, "timeout_ms", json_integer((json_int_t)r->timeout_ms));
    return o;
}

/**
 * @brief GET /admin/v1/shadow/rules: list all traffic shadow and canary rules.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_rules_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    shadow_rule_t   rules[128];
    int             n = 0;
    if (ops != NULL && ops->list_shadow_rules != NULL) {
        if (ops->list_shadow_rules(ops->ctx, rules, 128, &n) != 0) {
            return finish_error(
                status, body, len, 500, "internal_error", "failed to list shadow rules");
        }
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_array_append_new(arr, shadow_rule_to_json(&rules[i]));
    }
    json_t* out = json_object();
    json_object_set_new(out, "rules", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief POST /admin/v1/shadow/rules: create a new shadow or canary rule.
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_rules_create(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* src_m = jstring(jbody, "source_model", NULL);
    const char* tgt_m = jstring(jbody, "target_model", NULL);
    if (src_m == NULL || src_m[0] == '\0' || tgt_m == NULL || tgt_m[0] == '\0') {
        json_decref(jbody);
        return finish_error(
            status, body, len, 400, "bad_request", "source_model and target_model are required");
    }

    shadow_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    snprintf(rule.source_model, sizeof(rule.source_model), "%s", src_m);
    snprintf(rule.target_model, sizeof(rule.target_model), "%s", tgt_m);

    const char* tgt_prov = jstring(jbody, "target_provider", "openai");
    snprintf(rule.target_provider,
             sizeof(rule.target_provider),
             "%s",
             tgt_prov[0] ? tgt_prov : "openai");

    const char* mode_str = jstring(jbody, "mode", "SHADOW");
    if (strcasecmp(mode_str, "CANARY") == 0) {
        rule.mode = TRAFFIC_MODE_CANARY;
    } else {
        json_t* jm = json_object_get(jbody, "mode");
        if (jm != NULL && json_is_integer(jm) && json_integer_value(jm) == 1) {
            rule.mode = TRAFFIC_MODE_CANARY;
        } else {
            rule.mode = TRAFFIC_MODE_SHADOW;
        }
    }

    rule.sample_rate = 1.0;
    json_t* jsr = json_object_get(jbody, "sample_rate");
    if (jsr != NULL) {
        if (json_is_real(jsr)) {
            rule.sample_rate = json_real_value(jsr);
        } else if (json_is_integer(jsr)) {
            rule.sample_rate = (double)json_integer_value(jsr);
        }
    }
    if (rule.sample_rate < 0.0) {
        rule.sample_rate = 0.0;
    }
    if (rule.sample_rate > 1.0) {
        rule.sample_rate = 1.0;
    }

    const char* hm = jstring(jbody, "header_match", "");
    snprintf(rule.header_match, sizeof(rule.header_match), "%s", hm);

    rule.enabled = true;
    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        rule.enabled = json_is_true(jen);
    }

    rule.timeout_ms = 30000;
    json_t* jto = json_object_get(jbody, "timeout_ms");
    if (jto != NULL && json_is_integer(jto)) {
        rule.timeout_ms = (uint32_t)json_integer_value(jto);
    }

    json_decref(jbody);

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->create_shadow_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }
    long out_id = 0;
    if (ops->create_shadow_rule(ops->ctx, &rule, &out_id) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to create shadow rule");
    }
    rule.id = out_id;

    if (adm->ac != NULL) {
        aigate_core_reload_shadow_rules(adm->ac);
    }

    return finish_json(status, body, len, 201, shadow_rule_to_json(&rule));
}

/**
 * @brief PUT/PATCH /admin/v1/shadow/rules/:id: update an existing shadow or canary rule.
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param rest     Target rule ID string.
 * @param req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_rules_update(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->list_shadow_rules == NULL || ops->update_shadow_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }

    shadow_rule_t rules[128];
    int           n = 0;
    if (ops->list_shadow_rules(ops->ctx, rules, 128, &n) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to list shadow rules");
    }

    shadow_rule_t* target = NULL;
    for (int i = 0; i < n; i++) {
        if (rules[i].id == id) {
            target = &rules[i];
            break;
        }
    }
    if (target == NULL) {
        return finish_error(status, body, len, 404, "rule_not_found", "shadow rule not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* src_m = jstring(jbody, "source_model", NULL);
    if (src_m != NULL && src_m[0] != '\0') {
        snprintf(target->source_model, sizeof(target->source_model), "%s", src_m);
    }
    const char* tgt_m = jstring(jbody, "target_model", NULL);
    if (tgt_m != NULL && tgt_m[0] != '\0') {
        snprintf(target->target_model, sizeof(target->target_model), "%s", tgt_m);
    }
    const char* tgt_prov = jstring(jbody, "target_provider", NULL);
    if (tgt_prov != NULL && tgt_prov[0] != '\0') {
        snprintf(target->target_provider, sizeof(target->target_provider), "%s", tgt_prov);
    }

    const char* mode_str = jstring(jbody, "mode", NULL);
    if (mode_str != NULL) {
        if (strcasecmp(mode_str, "CANARY") == 0) {
            target->mode = TRAFFIC_MODE_CANARY;
        } else {
            target->mode = TRAFFIC_MODE_SHADOW;
        }
    } else {
        json_t* jm = json_object_get(jbody, "mode");
        if (jm != NULL && json_is_integer(jm)) {
            target->mode = json_integer_value(jm) == 1 ? TRAFFIC_MODE_CANARY : TRAFFIC_MODE_SHADOW;
        }
    }

    json_t* jsr = json_object_get(jbody, "sample_rate");
    if (jsr != NULL) {
        if (json_is_real(jsr)) {
            target->sample_rate = json_real_value(jsr);
        } else if (json_is_integer(jsr)) {
            target->sample_rate = (double)json_integer_value(jsr);
        }
        if (target->sample_rate < 0.0) {
            target->sample_rate = 0.0;
        }
        if (target->sample_rate > 1.0) {
            target->sample_rate = 1.0;
        }
    }

    const char* hm = jstring(jbody, "header_match", NULL);
    if (hm != NULL) {
        snprintf(target->header_match, sizeof(target->header_match), "%s", hm);
    }

    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        target->enabled = json_is_true(jen);
    }

    json_t* jto = json_object_get(jbody, "timeout_ms");
    if (jto != NULL && json_is_integer(jto)) {
        target->timeout_ms = (uint32_t)json_integer_value(jto);
    }

    json_decref(jbody);

    if (ops->update_shadow_rule(ops->ctx, target) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to update shadow rule");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_shadow_rules(adm->ac);
    }

    return finish_json(status, body, len, 200, shadow_rule_to_json(target));
}

/**
 * @brief DELETE /admin/v1/shadow/rules/:id: delete a shadow or canary rule.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @param rest   Target rule ID string.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_rules_delete(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    long id = atol(rest);
    if (id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->delete_shadow_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }

    if (ops->delete_shadow_rule(ops->ctx, id) != 0) {
        return finish_error(status, body, len, 404, "rule_not_found", "shadow rule not found");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_shadow_rules(adm->ac);
    }

    json_t* out = json_object();
    json_object_set_new(out, "deleted", json_true());
    json_object_set_new(out, "id", json_integer((json_int_t)id));
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/shadow/evaluations: list recent dual-track evaluation snapshots.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_evaluations_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    json_t* arr = json_array();
    if (adm->ac != NULL && adm->ac->shadow_eng != NULL) {
        shadow_eval_item_t items[100];
        int                count = shadow_engine_get_recent_evals(adm->ac->shadow_eng, items, 100);
        for (int i = 0; i < count; i++) {
            const shadow_eval_item_t* it = &items[i];
            json_t*                   o = json_object();
            json_object_set_new(o, "eval_id", json_string(it->eval_id));
            json_object_set_new(o, "trace_id", json_string(it->trace_id));
            json_object_set_new(o, "source_model", json_string(it->source_model));
            json_object_set_new(o, "target_model", json_string(it->target_model));
            json_object_set_new(
                o, "mode", json_string(it->mode == TRAFFIC_MODE_CANARY ? "CANARY" : "SHADOW"));
            json_object_set_new(o, "primary_latency_ms", json_real(it->primary_latency_ms));
            json_object_set_new(o, "shadow_latency_ms", json_real(it->shadow_latency_ms));
            json_object_set_new(o, "primary_ttft_ms", json_real(it->primary_ttft_ms));
            json_object_set_new(o, "shadow_ttft_ms", json_real(it->shadow_ttft_ms));
            json_object_set_new(o, "primary_http_status", json_integer(it->primary_http_status));
            json_object_set_new(o, "shadow_http_status", json_integer(it->shadow_http_status));
            json_object_set_new(o, "primary_tokens", json_integer((json_int_t)it->primary_tokens));
            json_object_set_new(o, "shadow_tokens", json_integer((json_int_t)it->shadow_tokens));
            json_object_set_new(o, "primary_cost_usd", json_real(it->primary_cost_usd));
            json_object_set_new(o, "shadow_cost_usd", json_real(it->shadow_cost_usd));
            json_object_set_new(o, "prompt_preview", json_string(it->prompt_preview));
            json_object_set_new(o, "primary_resp_snippet", json_string(it->primary_resp_snippet));
            json_object_set_new(o, "shadow_resp_snippet", json_string(it->shadow_resp_snippet));
            json_object_set_new(o, "timestamp_us", json_integer((json_int_t)it->timestamp_us));
            json_array_append_new(arr, o);
        }
    }
    json_t* out = json_object();
    json_object_set_new(out, "evaluations", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/shadow/stats: retrieve aggregate evaluation metrics.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_shadow_stats_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    shadow_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (adm->ac != NULL && adm->ac->shadow_eng != NULL) {
        shadow_engine_get_stats(adm->ac->shadow_eng, &stats);
    }
    json_t* out = json_object();
    json_object_set_new(out, "total_evaluated", json_integer((json_int_t)stats.total_evaluated));
    json_object_set_new(
        out, "successful_shadow", json_integer((json_int_t)stats.successful_shadow));
    json_object_set_new(out, "failed_shadow", json_integer((json_int_t)stats.failed_shadow));
    json_object_set_new(out, "primary_cost_usd", json_real(stats.primary_cost_usd));
    json_object_set_new(out, "shadow_cost_usd", json_real(stats.shadow_cost_usd));
    json_object_set_new(out, "cost_saved_usd", json_real(stats.cost_saved_usd));
    json_object_set_new(out, "avg_primary_lat_ms", json_real(stats.avg_primary_lat_ms));
    json_object_set_new(out, "avg_shadow_lat_ms", json_real(stats.avg_shadow_lat_ms));
    json_object_set_new(
        out, "dropped_shadow_requests", json_integer((json_int_t)stats.dropped_shadow_requests));
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief Convert compressor_rule_t to json_t object representation.
 * @param rule Pointer to compressor rule structure.
 * @return Allocated json_t object reference.
 */
static json_t*
compressor_rule_to_json(const compressor_rule_t* rule)
{
    json_t* o = json_object();
    json_object_set_new(o, "id", json_string(rule->id));
    json_object_set_new(o, "model_pattern", json_string(rule->model_pattern));
    json_object_set_new(o, "enabled", rule->enabled ? json_true() : json_false());
    json_object_set_new(o, "level", json_integer((json_int_t)rule->level));
    json_object_set_new(o, "min_tokens", json_integer((json_int_t)rule->min_tokens));
    json_object_set_new(o, "max_history_turns", json_integer((json_int_t)rule->max_history_turns));
    json_object_set_new(o, "target_ratio", json_real(rule->target_ratio));
    json_object_set_new(o, "preserve_system", rule->preserve_system ? json_true() : json_false());
    json_object_set_new(o, "preserve_code", rule->preserve_code ? json_true() : json_false());
    json_object_set_new(o, "preserve_tools", rule->preserve_tools ? json_true() : json_false());
    json_object_set_new(o, "created_at", json_integer((json_int_t)rule->created_at));
    json_object_set_new(o, "updated_at", json_integer((json_int_t)rule->updated_at));
    return o;
}

/**
 * @brief GET /admin/v1/compressor/rules: list all prompt compressor rules.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_rules_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    const pg_ops_t*   ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    compressor_rule_t rules[64];
    int               n = 0;
    if (ops != NULL && ops->list_compressor_rules != NULL) {
        if (ops->list_compressor_rules(ops->ctx, rules, 64, &n) != 0) {
            return finish_error(
                status, body, len, 500, "internal_error", "failed to list compressor rules");
        }
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_array_append_new(arr, compressor_rule_to_json(&rules[i]));
    }
    json_t* out = json_object();
    json_object_set_new(out, "rules", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief POST /admin/v1/compressor/rules: create a new prompt compressor rule.
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_rules_create(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* pat = jstring(jbody, "model_pattern", NULL);
    if (pat == NULL || pat[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "model_pattern is required");
    }

    compressor_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    const char* id_in = jstring(jbody, "id", NULL);
    if (id_in != NULL && id_in[0] != '\0') {
        snprintf(rule.id, sizeof(rule.id), "%s", id_in);
    } else {
        snprintf(rule.id,
                 sizeof(rule.id),
                 "rule-%08x%04x",
                 (uint32_t)rand(),
                 (uint32_t)(rand() & 0xffff));
    }

    snprintf(rule.model_pattern, sizeof(rule.model_pattern), "%s", pat);

    rule.enabled = true;
    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        rule.enabled = json_is_true(jen);
    }

    rule.level = COMPRESS_LEVEL_MODERATE;
    json_t* jlvl = json_object_get(jbody, "level");
    if (jlvl != NULL && json_is_integer(jlvl)) {
        rule.level = (compressor_level_t)json_integer_value(jlvl);
    }

    rule.min_tokens = 2048;
    json_t* jmin = json_object_get(jbody, "min_tokens");
    if (jmin != NULL && json_is_integer(jmin)) {
        rule.min_tokens = (uint32_t)json_integer_value(jmin);
    }

    rule.max_history_turns = 6;
    json_t* jmht = json_object_get(jbody, "max_history_turns");
    if (jmht != NULL && json_is_integer(jmht)) {
        rule.max_history_turns = (uint32_t)json_integer_value(jmht);
    }

    rule.target_ratio = 0.60;
    json_t* jtr = json_object_get(jbody, "target_ratio");
    if (jtr != NULL) {
        if (json_is_real(jtr)) {
            rule.target_ratio = json_real_value(jtr);
        } else if (json_is_integer(jtr)) {
            rule.target_ratio = (double)json_integer_value(jtr);
        }
    }

    rule.preserve_system = true;
    json_t* jps = json_object_get(jbody, "preserve_system");
    if (jps != NULL && json_is_boolean(jps)) {
        rule.preserve_system = json_is_true(jps);
    }

    rule.preserve_code = true;
    json_t* jpc = json_object_get(jbody, "preserve_code");
    if (jpc != NULL && json_is_boolean(jpc)) {
        rule.preserve_code = json_is_true(jpc);
    }

    rule.preserve_tools = true;
    json_t* jpt = json_object_get(jbody, "preserve_tools");
    if (jpt != NULL && json_is_boolean(jpt)) {
        rule.preserve_tools = json_is_true(jpt);
    }

    time_t now = time(NULL);
    rule.created_at = (int64_t)now;
    rule.updated_at = (int64_t)now;

    json_decref(jbody);

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->upsert_compressor_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }
    if (ops->upsert_compressor_rule(ops->ctx, &rule) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to create compressor rule");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_compressor_rules(adm->ac);
    }

    return finish_json(status, body, len, 201, compressor_rule_to_json(&rule));
}

/**
 * @brief PUT/PATCH /admin/v1/compressor/rules/:id: update an existing compressor rule.
 * @param adm      Admin context pointer.
 * @param status   Pointer to store HTTP response status.
 * @param body     Pointer to store response body string.
 * @param len      Pointer to store response body length.
 * @param rest     Target rule ID string.
 * @param req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_rules_update(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    if (rest == NULL || rest[0] == '\0') {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->list_compressor_rules == NULL || ops->upsert_compressor_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }

    compressor_rule_t rules[64];
    int               n = 0;
    if (ops->list_compressor_rules(ops->ctx, rules, 64, &n) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to list compressor rules");
    }

    compressor_rule_t* target = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(rules[i].id, rest) == 0) {
            target = &rules[i];
            break;
        }
    }
    if (target == NULL) {
        return finish_error(status, body, len, 404, "rule_not_found", "compressor rule not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* pat = jstring(jbody, "model_pattern", NULL);
    if (pat != NULL && pat[0] != '\0') {
        snprintf(target->model_pattern, sizeof(target->model_pattern), "%s", pat);
    }

    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        target->enabled = json_is_true(jen);
    }

    json_t* jlvl = json_object_get(jbody, "level");
    if (jlvl != NULL && json_is_integer(jlvl)) {
        target->level = (compressor_level_t)json_integer_value(jlvl);
    }

    json_t* jmin = json_object_get(jbody, "min_tokens");
    if (jmin != NULL && json_is_integer(jmin)) {
        target->min_tokens = (uint32_t)json_integer_value(jmin);
    }

    json_t* jmht = json_object_get(jbody, "max_history_turns");
    if (jmht != NULL && json_is_integer(jmht)) {
        target->max_history_turns = (uint32_t)json_integer_value(jmht);
    }

    json_t* jtr = json_object_get(jbody, "target_ratio");
    if (jtr != NULL) {
        if (json_is_real(jtr)) {
            target->target_ratio = json_real_value(jtr);
        } else if (json_is_integer(jtr)) {
            target->target_ratio = (double)json_integer_value(jtr);
        }
    }

    json_t* jps = json_object_get(jbody, "preserve_system");
    if (jps != NULL && json_is_boolean(jps)) {
        target->preserve_system = json_is_true(jps);
    }

    json_t* jpc = json_object_get(jbody, "preserve_code");
    if (jpc != NULL && json_is_boolean(jpc)) {
        target->preserve_code = json_is_true(jpc);
    }

    json_t* jpt = json_object_get(jbody, "preserve_tools");
    if (jpt != NULL && json_is_boolean(jpt)) {
        target->preserve_tools = json_is_true(jpt);
    }

    target->updated_at = (int64_t)time(NULL);

    json_decref(jbody);

    if (ops->upsert_compressor_rule(ops->ctx, target) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to update compressor rule");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_compressor_rules(adm->ac);
    }

    return finish_json(status, body, len, 200, compressor_rule_to_json(target));
}

/**
 * @brief DELETE /admin/v1/compressor/rules/:id: delete a prompt compressor rule.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @param rest   Target rule ID string.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_rules_delete(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    if (rest == NULL || rest[0] == '\0') {
        return finish_error(status, body, len, 400, "bad_request", "invalid rule id");
    }

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->delete_compressor_rule == NULL) {
        return finish_error(status, body, len, 500, "internal_error", "store ops unavailable");
    }

    if (ops->delete_compressor_rule(ops->ctx, rest) != 0) {
        return finish_error(status, body, len, 404, "rule_not_found", "compressor rule not found");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_compressor_rules(adm->ac);
    }

    json_t* out = json_object();
    json_object_set_new(out, "deleted", json_true());
    json_object_set_new(out, "id", json_string(rest));
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/compressor/snapshots: list recent prompt compression snapshots.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_snapshots_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    json_t* arr = json_array();
    if (adm->ac != NULL && adm->ac->comp_cache != NULL) {
        compressor_snapshot_t snaps[200];
        size_t count = compressor_cache_get_snapshots(adm->ac->comp_cache, snaps, 200);
        for (size_t i = 0; i < count; i++) {
            const compressor_snapshot_t* s = &snaps[i];
            json_t*                      o = json_object();
            json_object_set_new(o, "req_id", json_string(s->req_id));
            json_object_set_new(o, "model", json_string(s->model));
            json_object_set_new(o, "timestamp", json_integer((json_int_t)s->timestamp));
            json_object_set_new(o, "original_tokens", json_integer((json_int_t)s->original_tokens));
            json_object_set_new(
                o, "compressed_tokens", json_integer((json_int_t)s->compressed_tokens));
            json_object_set_new(o, "saved_tokens", json_integer((json_int_t)s->saved_tokens));
            json_object_set_new(o, "compression_ratio", json_real(s->compression_ratio));
            json_object_set_new(o, "elapsed_us", json_integer((json_int_t)s->elapsed_us));
            json_object_set_new(o, "prompt_preview", json_string(s->prompt_preview));
            json_object_set_new(o, "orig_preview", json_string(s->orig_preview));
            json_object_set_new(o, "comp_preview", json_string(s->comp_preview));
            json_array_append_new(arr, o);
        }
    }
    json_t* out = json_object();
    json_object_set_new(out, "snapshots", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/compressor/stats: retrieve aggregate prompt compression metrics.
 * @param adm    Admin context pointer.
 * @param status Pointer to store HTTP response status.
 * @param body   Pointer to store response body string.
 * @param len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_compressor_stats_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    compressor_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (adm->ac != NULL && adm->ac->comp_cache != NULL) {
        compressor_cache_get_stats(adm->ac->comp_cache, &stats);
    }
    json_t* out = json_object();
    json_object_set_new(out, "total_evaluated", json_integer((json_int_t)stats.total_evaluated));
    json_object_set_new(out, "total_compressed", json_integer((json_int_t)stats.total_compressed));
    json_object_set_new(
        out, "total_orig_tokens", json_integer((json_int_t)stats.total_orig_tokens));
    json_object_set_new(
        out, "total_comp_tokens", json_integer((json_int_t)stats.total_comp_tokens));
    json_object_set_new(
        out, "total_saved_tokens", json_integer((json_int_t)stats.total_saved_tokens));
    json_object_set_new(
        out, "total_duration_us", json_integer((json_int_t)stats.total_duration_us));
    json_object_set_new(out, "estimated_cost_saved", json_real(stats.estimated_cost_saved));
    double avg_ratio = (stats.total_orig_tokens > 0)
                           ? (double)stats.total_comp_tokens / (double)stats.total_orig_tokens
                           : 1.0;
    json_object_set_new(out, "avg_compression_ratio", json_real(avg_ratio));
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief Convert cache_optimizer_rule_t to json_t object representation.
 * @param[in] rule Pointer to cache optimizer rule structure.
 * @return Allocated json_t object reference.
 */
static json_t*
cache_optimizer_rule_to_json(const cache_optimizer_rule_t* rule)
{
    json_t* o = json_object();
    json_object_set_new(o, "id", json_string(rule->id));
    json_object_set_new(o, "model_pattern", json_string(rule->model_pattern));
    json_object_set_new(o, "enabled", rule->enabled ? json_true() : json_false());
    json_object_set_new(o, "sort_tools", rule->sort_tools ? json_true() : json_false());
    json_object_set_new(
        o, "sink_dynamic_system", rule->sink_dynamic_system ? json_true() : json_false());
    json_object_set_new(o,
                        "inject_anthropic_breakpoints",
                        rule->inject_anthropic_breakpoints ? json_true() : json_false());
    json_object_set_new(
        o, "min_tokens_threshold", json_integer((json_int_t)rule->min_tokens_threshold));
    json_object_set_new(o, "created_at", json_integer((json_int_t)rule->created_at));
    json_object_set_new(o, "updated_at", json_integer((json_int_t)rule->updated_at));
    return o;
}

/**
 * @brief GET /admin/v1/cache-optimizer/rules: list all prompt cache optimizer rules.
 * @param[in]  adm    Admin context pointer.
 * @param[out] status Pointer to store HTTP response status.
 * @param[out] body   Pointer to store response body string.
 * @param[out] len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_rules_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    const pg_ops_t*        ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    cache_optimizer_rule_t rules[64];
    int                    n = 0;
    if (ops != NULL && ops->list_cache_optimizer_rules != NULL) {
        if (ops->list_cache_optimizer_rules(ops->ctx, rules, 64, &n) != 0) {
            return finish_error(
                status, body, len, 500, "internal_error", "failed to list cache optimizer rules");
        }
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_array_append_new(arr, cache_optimizer_rule_to_json(&rules[i]));
    }
    json_t* out = json_object();
    json_object_set_new(out, "rules", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief POST /admin/v1/cache-optimizer/rules: create a new prompt cache optimizer rule.
 * @param[in]  adm      Admin context pointer.
 * @param[out] status   Pointer to store HTTP response status.
 * @param[out] body     Pointer to store response body string.
 * @param[out] len      Pointer to store response body length.
 * @param[in]  req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_rules_create(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)
{
    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* pat = jstring(jbody, "model_pattern", NULL);
    if (pat == NULL || pat[0] == '\0') {
        json_decref(jbody);
        return finish_error(status, body, len, 400, "bad_request", "model_pattern is required");
    }

    cache_optimizer_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    const char* id_in = jstring(jbody, "id", NULL);
    if (id_in != NULL && id_in[0] != '\0') {
        snprintf(rule.id, sizeof(rule.id), "%s", id_in);
    } else {
        snprintf(rule.id,
                 sizeof(rule.id),
                 "rule-%08x%04x",
                 (uint32_t)rand(),
                 (uint32_t)(rand() & 0xffff));
    }

    snprintf(rule.model_pattern, sizeof(rule.model_pattern), "%s", pat);

    rule.enabled = true;
    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        rule.enabled = json_is_true(jen);
    }

    rule.sort_tools = true;
    json_t* jsort = json_object_get(jbody, "sort_tools");
    if (jsort != NULL && json_is_boolean(jsort)) {
        rule.sort_tools = json_is_true(jsort);
    }

    rule.sink_dynamic_system = true;
    json_t* jsink = json_object_get(jbody, "sink_dynamic_system");
    if (jsink != NULL && json_is_boolean(jsink)) {
        rule.sink_dynamic_system = json_is_true(jsink);
    }

    rule.inject_anthropic_breakpoints = true;
    json_t* jbp = json_object_get(jbody, "inject_anthropic_breakpoints");
    if (jbp != NULL && json_is_boolean(jbp)) {
        rule.inject_anthropic_breakpoints = json_is_true(jbp);
    }

    rule.min_tokens_threshold = 1024;
    json_t* jmin = json_object_get(jbody, "min_tokens_threshold");
    if (jmin != NULL && json_is_integer(jmin)) {
        rule.min_tokens_threshold = (uint32_t)json_integer_value(jmin);
    }

    rule.created_at = (int64_t)time(NULL);
    rule.updated_at = rule.created_at;

    json_decref(jbody);

    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->upsert_cache_optimizer_rule == NULL) {
        return finish_error(
            status, body, len, 503, "service_unavailable", "database store not available");
    }
    if (ops->upsert_cache_optimizer_rule(ops->ctx, &rule) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to create cache optimizer rule");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_cache_optimizer_rules(adm->ac);
    }

    return finish_json(status, body, len, 201, cache_optimizer_rule_to_json(&rule));
}

/**
 * @brief PUT/PATCH /admin/v1/cache-optimizer/rules/:id: update an existing cache optimizer rule.
 * @param[in]  adm      Admin context pointer.
 * @param[out] status   Pointer to store HTTP response status.
 * @param[out] body     Pointer to store response body string.
 * @param[out] len      Pointer to store response body length.
 * @param[in]  rest     Rule ID extracted from URI.
 * @param[in]  req_body Raw request payload buffer.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_rules_update(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest, const void* req_body)
{
    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->list_cache_optimizer_rules == NULL ||
        ops->upsert_cache_optimizer_rule == NULL) {
        return finish_error(
            status, body, len, 503, "service_unavailable", "database store not available");
    }

    cache_optimizer_rule_t rules[64];
    int                    n = 0;
    if (ops->list_cache_optimizer_rules(ops->ctx, rules, 64, &n) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to list cache optimizer rules");
    }

    cache_optimizer_rule_t* target = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(rules[i].id, rest) == 0) {
            target = &rules[i];
            break;
        }
    }
    if (target == NULL) {
        return finish_error(
            status, body, len, 404, "rule_not_found", "cache optimizer rule not found");
    }

    json_t* jbody = parse_body(req_body, 0);
    if (jbody == NULL) {
        return finish_error(status, body, len, 400, "bad_request", "invalid json body");
    }

    const char* pat = jstring(jbody, "model_pattern", NULL);
    if (pat != NULL && pat[0] != '\0') {
        snprintf(target->model_pattern, sizeof(target->model_pattern), "%s", pat);
    }

    json_t* jen = json_object_get(jbody, "enabled");
    if (jen != NULL && json_is_boolean(jen)) {
        target->enabled = json_is_true(jen);
    }

    json_t* jsort = json_object_get(jbody, "sort_tools");
    if (jsort != NULL && json_is_boolean(jsort)) {
        target->sort_tools = json_is_true(jsort);
    }

    json_t* jsink = json_object_get(jbody, "sink_dynamic_system");
    if (jsink != NULL && json_is_boolean(jsink)) {
        target->sink_dynamic_system = json_is_true(jsink);
    }

    json_t* jbp = json_object_get(jbody, "inject_anthropic_breakpoints");
    if (jbp != NULL && json_is_boolean(jbp)) {
        target->inject_anthropic_breakpoints = json_is_true(jbp);
    }

    json_t* jmin = json_object_get(jbody, "min_tokens_threshold");
    if (jmin != NULL && json_is_integer(jmin)) {
        target->min_tokens_threshold = (uint32_t)json_integer_value(jmin);
    }

    target->updated_at = (int64_t)time(NULL);
    json_decref(jbody);

    if (ops->upsert_cache_optimizer_rule(ops->ctx, target) != 0) {
        return finish_error(
            status, body, len, 500, "internal_error", "failed to update cache optimizer rule");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_cache_optimizer_rules(adm->ac);
    }

    return finish_json(status, body, len, 200, cache_optimizer_rule_to_json(target));
}

/**
 * @brief DELETE /admin/v1/cache-optimizer/rules/:id: delete a prompt cache optimizer rule.
 * @param[in]  adm    Admin context pointer.
 * @param[out] status Pointer to store HTTP response status.
 * @param[out] body   Pointer to store response body string.
 * @param[out] len    Pointer to store response body length.
 * @param[in]  rest   Rule ID extracted from URI.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_rules_delete(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest)
{
    const pg_ops_t* ops = adm->ps != NULL ? pg_store_ops(adm->ps) : NULL;
    if (ops == NULL || ops->delete_cache_optimizer_rule == NULL) {
        return finish_error(
            status, body, len, 503, "service_unavailable", "database store not available");
    }

    if (ops->delete_cache_optimizer_rule(ops->ctx, rest) != 0) {
        return finish_error(
            status, body, len, 404, "rule_not_found", "cache optimizer rule not found");
    }

    if (adm->ac != NULL) {
        aigate_core_reload_cache_optimizer_rules(adm->ac);
    }

    json_t* out = json_object();
    json_object_set_new(out, "deleted", json_true());
    json_object_set_new(out, "id", json_string(rest));
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/cache-optimizer/snapshots: list recent prompt cache optimizer snapshots.
 * @param[in]  adm    Admin context pointer.
 * @param[out] status Pointer to store HTTP response status.
 * @param[out] body   Pointer to store response body string.
 * @param[out] len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_snapshots_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    json_t* arr = json_array();
    if (adm->ac != NULL && adm->ac->cache_opt_cache != NULL) {
        cache_optimizer_snapshot_t snaps[200];
        size_t count = cache_optimizer_cache_get_snapshots(adm->ac->cache_opt_cache, snaps, 200);
        for (size_t i = 0; i < count; i++) {
            const cache_optimizer_snapshot_t* s = &snaps[i];
            json_t*                           o = json_object();
            json_object_set_new(o, "req_id", json_string(s->req_id));
            json_object_set_new(o, "model", json_string(s->model));
            json_object_set_new(o, "timestamp", json_integer((json_int_t)s->timestamp));
            json_object_set_new(
                o, "upstream_cache_hit", s->upstream_cache_hit ? json_true() : json_false());
            json_object_set_new(o, "prompt_tokens", json_integer((json_int_t)s->prompt_tokens));
            json_object_set_new(o, "cached_tokens", json_integer((json_int_t)s->cached_tokens));
            json_object_set_new(o, "cost_savings_usd", json_real(s->cost_savings_usd));
            json_object_set_new(o, "latency_us", json_integer((json_int_t)s->latency_us));
            json_object_set_new(
                o, "breakpoints_count", json_integer((json_int_t)s->breakpoints_count));
            json_object_set_new(o, "dynamic_sunk", s->dynamic_sunk ? json_true() : json_false());
            json_object_set_new(o, "tools_sorted", s->tools_sorted ? json_true() : json_false());
            json_array_append_new(arr, o);
        }
    }
    json_t* out = json_object();
    json_object_set_new(out, "snapshots", arr);
    return finish_json(status, body, len, 200, out);
}

/**
 * @brief GET /admin/v1/cache-optimizer/stats: retrieve aggregate prompt cache optimizer metrics.
 * @param[in]  adm    Admin context pointer.
 * @param[out] status Pointer to store HTTP response status.
 * @param[out] body   Pointer to store response body string.
 * @param[out] len    Pointer to store response body length.
 * @return 0 on success, negative on internal error.
 */
static int
admin_cache_optimizer_stats_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    cache_optimizer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (adm->ac != NULL && adm->ac->cache_opt_cache != NULL) {
        cache_optimizer_cache_get_stats(adm->ac->cache_opt_cache, &stats);
    }
    json_t* out = json_object();
    json_object_set_new(
        out, "total_optimized_requests", json_integer((json_int_t)stats.total_optimized_requests));
    json_object_set_new(out,
                        "upstream_cache_hit_requests",
                        json_integer((json_int_t)stats.upstream_cache_hit_requests));
    json_object_set_new(
        out, "total_prompt_tokens", json_integer((json_int_t)stats.total_prompt_tokens));
    json_object_set_new(
        out, "total_cached_tokens", json_integer((json_int_t)stats.total_cached_tokens));
    json_object_set_new(out, "total_savings_usd", json_real(stats.total_savings_usd));
    json_object_set_new(out, "avg_latency_us", json_integer((json_int_t)stats.avg_latency_us));
    double hit_rate =
        (stats.total_optimized_requests > 0)
            ? (double)stats.upstream_cache_hit_requests / (double)stats.total_optimized_requests
            : 0.0;
    json_object_set_new(out, "cache_hit_rate", json_real(hit_rate));
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ audit & SLA */

static int
admin_audit_events_get(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    size_t   limit = 50;
    uint64_t after_seq = 0;
    if (query != NULL) {
        char limit_str[32] = {0};
        if (query_param(query, "limit", limit_str, sizeof(limit_str)) == 0) {
            long val = atol(limit_str);
            if (val > 0) {
                limit = (size_t)val;
                if (limit > 200) {
                    limit = 200;
                }
            }
        }
        char after_str[32] = {0};
        if (query_param(query, "after_seq", after_str, sizeof(after_str)) == 0) {
            after_seq = (uint64_t)strtoull(after_str, NULL, 10);
        }
    }

    json_t*  root = json_object();
    json_t*  arr = json_array();
    uint64_t max_seq = 0;
    size_t   missed = 0;
    if (adm->ac != NULL && adm->ac->audit != NULL) {
        audit_live_event_t events[200];
        size_t count = audit_logger_query_recent(adm->ac->audit, events, limit, after_seq, &missed);
        for (size_t i = 0; i < count; i++) {
            if (events[i].seq_id > max_seq) {
                max_seq = events[i].seq_id;
            }
            json_t* item = json_object();
            json_object_set_new(item, "seq_id", json_integer((json_int_t)events[i].seq_id));
            json_object_set_new(item, "trace_id", json_string(events[i].trace_id));
            json_object_set_new(item, "tenant_id", json_string(events[i].tenant_id));
            json_object_set_new(item, "client_ip", json_string(events[i].client_ip));
            json_object_set_new(item, "model", json_string(events[i].model));
            json_object_set_new(item, "requested_model", json_string(events[i].model));
            json_object_set_new(item, "routed_model", json_string(events[i].routed_model));
            json_object_set_new(item, "provider", json_string(events[i].provider));
            json_object_set_new(item, "http_status", json_integer(events[i].http_status));
            json_object_set_new(item, "prompt_tokens", json_integer(events[i].prompt_tokens));
            json_object_set_new(
                item, "completion_tokens", json_integer(events[i].completion_tokens));
            json_object_set_new(item, "ttft_ms", json_integer(events[i].ttft_ms));
            json_object_set_new(item, "total_latency_ms", json_integer(events[i].total_latency_ms));
            json_object_set_new(
                item, "severity", json_string(audit_severity_str(events[i].severity)));
            json_object_set_new(item, "violation_type", json_string(events[i].violation_type));
            json_object_set_new(item, "rule_detail", json_string(events[i].rule_detail));
            json_object_set_new(
                item,
                "rule_tag",
                json_string(
                    events[i].violation_type[0]
                        ? events[i].violation_type
                        : (events[i].fallback_reason[0] ? events[i].fallback_reason : "NONE")));
            json_object_set_new(item, "fallback_reason", json_string(events[i].fallback_reason));
            json_object_set_new(item, "prompt_snippet", json_string(events[i].prompt_snippet));
            json_object_set_new(item, "timestamp_ms", json_integer(events[i].timestamp_ms));
            json_array_append_new(arr, item);
        }
    }
    json_object_set_new(root, "status", json_string("ok"));
    json_object_set_new(root, "events", arr);
    json_object_set_new(root, "latest_seq", json_integer((json_int_t)max_seq));
    json_object_set_new(root, "missed_count", json_integer((json_int_t)missed));
    return finish_json(status, body, len, 200, root);
}

static int
admin_audit_violations_get(
    admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    char tenant_id[64] = {0};
    char rule_tag[64] = {0};
    char trace_id[64] = {0};
    int  limit = 50;
    int  offset = 0;

    if (query != NULL) {
        query_param(query, "tenant_id", tenant_id, sizeof(tenant_id));
        query_param(query, "rule_tag", rule_tag, sizeof(rule_tag));
        query_param(query, "trace_id", trace_id, sizeof(trace_id));
        char lim_buf[32] = {0};
        if (query_param(query, "limit", lim_buf, sizeof(lim_buf)) == 0) {
            int v = atoi(lim_buf);
            if (v > 0) {
                limit = v;
            }
            if (limit > 200) {
                limit = 200;
            }
        }
        char off_buf[32] = {0};
        if (query_param(query, "offset", off_buf, sizeof(off_buf)) == 0) {
            int v = atoi(off_buf);
            if (v >= 0) {
                offset = v;
            }
        }
    }

    json_t* root = json_object();
    json_t* items = json_array();

    if (adm->ps != NULL && pg_store_ops(adm->ps) != NULL &&
        pg_store_ops(adm->ps)->list_audit_violations != NULL) {
        audit_violation_record_t recs[200];
        int                      total = 0;
        int                      returned = 0;
        int rc = pg_store_list_audit_violations(adm->ps,
                                                tenant_id[0] ? tenant_id : NULL,
                                                rule_tag[0] ? rule_tag : NULL,
                                                trace_id[0] ? trace_id : NULL,
                                                limit,
                                                offset,
                                                recs,
                                                200,
                                                &total,
                                                &returned);
        if (rc == 0) {
            json_object_set_new(root, "total", json_integer(total));
            for (int i = 0; i < returned; i++) {
                json_t* it = json_object();
                json_object_set_new(it, "id", json_integer((json_int_t)recs[i].id));
                json_object_set_new(it, "trace_id", json_string(recs[i].trace_id));
                json_object_set_new(it, "tenant_id", json_string(recs[i].tenant_id));
                json_object_set_new(it, "client_ip", json_string(recs[i].client_ip));
                json_object_set_new(it, "model", json_string(recs[i].model));
                json_object_set_new(it, "routed_model", json_string(recs[i].routed_model));
                json_object_set_new(it, "http_status", json_integer(recs[i].http_status));
                json_object_set_new(it, "rule_tag", json_string(recs[i].rule_tag));
                json_object_set_new(it, "severity", json_string(recs[i].severity));
                json_object_set_new(it, "ttft_ms", json_integer(recs[i].ttft_ms));
                json_object_set_new(it, "total_latency_ms", json_integer(recs[i].total_latency_ms));
                json_object_set_new(it, "fallback_reason", json_string(recs[i].fallback_reason));
                if (recs[i].prompt_snapshot != NULL) {
                    json_object_set_new(
                        it, "prompt_snapshot", json_string(recs[i].prompt_snapshot));
                } else {
                    json_object_set_new(it, "prompt_snapshot", json_null());
                }
                if (recs[i].completion_snapshot != NULL) {
                    json_object_set_new(
                        it, "completion_snapshot", json_string(recs[i].completion_snapshot));
                } else {
                    json_object_set_new(it, "completion_snapshot", json_null());
                }
                json_object_set_new(it, "created_at", json_string(recs[i].created_at));
                json_array_append_new(items, it);
                audit_violation_record_free(&recs[i]);
            }
        } else {
            json_object_set_new(root, "total", json_integer(0));
        }
    } else {
        json_object_set_new(root, "total", json_integer(0));
    }

    json_object_set_new(root, "items", items);
    return finish_json(status, body, len, 200, root);
}

static const char*
admin_sla_state_str(cb_state_t st)
{
    switch (st) {
    case CB_CLOSED:
        return "HEALTHY";
    case CB_SLA_DEGRADED:
        return "SLA_DEGRADED";
    case CB_OPEN:
        return "OPEN";
    case CB_HALF_OPEN:
        return "HALF_OPEN";
    default:
        return "UNKNOWN";
    }
}

static int
admin_models_sla_get(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    json_t* root = json_object();
    json_t* arr = json_array();

    if (adm->ps != NULL && pg_store_ops(adm->ps) != NULL &&
        pg_store_ops(adm->ps)->list_models != NULL) {
        model_rec_t recs[256];
        int         n_models = 0;
        if (pg_store_ops(adm->ps)->list_models(pg_store_ops(adm->ps)->ctx, recs, 256, &n_models) ==
            0) {
            for (int i = 0; i < n_models; i++) {
                char       fallback[64] = {0};
                cb_state_t st = CB_CLOSED;
                uint32_t   avg_ttft = 0;
                if (adm->ac != NULL && adm->ac->cb != NULL) {
                    st = cb_get_sla_state(
                        adm->ac->cb, recs[i].name, NULL, fallback, sizeof(fallback));
                    avg_ttft = cb_get_sla_avg_ttft(adm->ac->cb, recs[i].name, NULL);
                }

                if (fallback[0] == '\0' && recs[i].default_params_json[0] != '\0') {
                    json_error_t jerr;
                    json_t*      jdp = json_loads(recs[i].default_params_json, 0, &jerr);
                    if (jdp != NULL) {
                        json_t* jfb_val = json_object_get(jdp, "fallback_model");
                        if (jfb_val != NULL && json_is_string(jfb_val)) {
                            snprintf(fallback, sizeof(fallback), "%s", json_string_value(jfb_val));
                            if (adm->ac != NULL && adm->ac->cb != NULL) {
                                cb_configure_sla(
                                    adm->ac->cb, recs[i].name, 3000, 6000, 10, 0.40f, fallback);
                            }
                        }
                        json_decref(jdp);
                    }
                }

                json_t* it = json_object();
                json_object_set_new(it, "model", json_string(recs[i].name));
                json_object_set_new(it, "sla_state", json_string(admin_sla_state_str(st)));
                json_object_set_new(it, "fallback_model", json_string(fallback));
                json_object_set_new(it, "current_avg_ttft_ms", json_integer(avg_ttft));
                json_array_append_new(arr, it);
                model_rec_free(&recs[i]);
            }
        }
    }

    json_object_set_new(root, "models", arr);
    return finish_json(status, body, len, 200, root);
}

static int
admin_model_sla_override(admin_ctx_t* adm,
                         int*         status,
                         char**       body,
                         size_t*      len,
                         const char*  model_subpath,
                         const void*  req_body)
{
    const char* p = strstr(model_subpath, "/sla/override");
    if (p == NULL) {
        return finish_error(status, body, len, 404, "not_found", "endpoint not found");
    }
    size_t name_len = (size_t)(p - model_subpath);
    if (name_len == 0 || name_len >= 64) {
        return finish_error(status, body, len, 400, "bad_request", "invalid model identifier");
    }
    char model_id[64] = {0};
    memcpy(model_id, model_subpath, name_len);
    model_id[name_len] = '\0';

    json_error_t err;
    json_t*      jbody = json_loads(req_body ? (const char*)req_body : "", 0, &err);
    if (jbody == NULL || !json_is_object(jbody)) {
        if (jbody != NULL) {
            json_decref(jbody);
        }
        return finish_error(status, body, len, 400, "bad_request", "invalid JSON body");
    }

    json_t*     jaction = json_object_get(jbody, "action");
    const char* action =
        (jaction != NULL && json_is_string(jaction)) ? json_string_value(jaction) : "";

    cb_state_t new_st = CB_CLOSED;
    if (strcmp(action, "degrade") == 0) {
        new_st = CB_SLA_DEGRADED;
    } else if (strcmp(action, "reset") == 0) {
        new_st = CB_CLOSED;
    } else if (strcmp(action, "open") == 0) {
        new_st = CB_OPEN;
    } else {
        json_decref(jbody);
        return finish_error(status,
                            body,
                            len,
                            400,
                            "bad_request",
                            "unknown action; must be degrade, reset, or open");
    }
    json_decref(jbody);

    if (adm->ac != NULL && adm->ac->cb != NULL) {
        char cur_fb[64] = {0};
        cb_get_sla_state(adm->ac->cb, model_id, NULL, cur_fb, sizeof(cur_fb));
        if (cur_fb[0] == '\0' && adm->ps != NULL && pg_store_ops(adm->ps)->get_model != NULL) {
            model_rec_t mrec;
            if (pg_store_ops(adm->ps)->get_model(pg_store_ops(adm->ps)->ctx, model_id, &mrec) ==
                0) {
                if (mrec.default_params_json[0] != '\0') {
                    json_error_t jerr;
                    json_t*      jdp = json_loads(mrec.default_params_json, 0, &jerr);
                    if (jdp != NULL) {
                        json_t* jfb_val = json_object_get(jdp, "fallback_model");
                        if (jfb_val != NULL && json_is_string(jfb_val)) {
                            cb_configure_sla(adm->ac->cb,
                                             model_id,
                                             3000,
                                             6000,
                                             10,
                                             0.40f,
                                             json_string_value(jfb_val));
                        }
                        json_decref(jdp);
                    }
                }
                model_rec_free(&mrec);
            }
        }
        cb_override_state(adm->ac->cb, model_id, NULL, new_st);
    }

    json_t* out = json_object();
    json_object_set_new(out, "status", json_string("ok"));
    json_object_set_new(out, "model", json_string(model_id));
    json_object_set_new(out, "action", json_string(action));
    json_object_set_new(out, "new_state", json_string(admin_sla_state_str(new_st)));
    json_object_set_new(out, "success", json_true());
    return finish_json(status, body, len, 200, out);
}

/* ------------------------------------------------------------ dispatch */

int
admin_dispatch(admin_ctx_t* adm,
               const char*  uri,
               const char*  method,
               const char*  client_ip,
               const char*  bearer,
               const void*  body,
               size_t       body_len,
               int*         out_status,
               char**       out_body,
               size_t*      out_len)
{
    (void)body_len;
    *out_status = 401;
    *out_body = NULL;
    *out_len = 0;

    /* lockout pre-check: skip the token work when the peer is inside a window */
    if (lockout_hit(client_ip)) {
        return finish_error(out_status,
                            out_body,
                            out_len,
                            429,
                            "locked_out",
                            "too many failed admin attempts; retry later");
    }

    if (!admin_auth_ok(adm, bearer)) {
        lockout_fail(client_ip);
        return finish_error(
            out_status, out_body, out_len, 401, "auth_error", "invalid admin token");
    }
    lockout_clear(client_ip);

    const char* qmark = strchr(uri, '?');
    char        path[256];
    if (qmark != NULL) {
        size_t n = (size_t)(qmark - uri);
        if (n >= sizeof path) {
            n = sizeof path - 1;
        }
        memcpy(path, uri, n);
        path[n] = '\0';
    } else {
        snprintf(path, sizeof path, "%s", uri);
    }
    const char* query = qmark != NULL ? qmark + 1 : NULL;

    /* strip /admin/v1/ prefix */
    const char* rest = path;
    const char* prefix = "/admin/v1/";
    size_t      plen = strlen(prefix);
    if (strncmp(path, prefix, plen) == 0) {
        rest = path + plen;
    }

    if (strncmp(rest, "keys", 4) == 0) {
        if (strcmp(rest, "keys") == 0) {
            if (strcmp(method, "POST") == 0) {
                return key_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return key_list(adm, out_status, out_body, out_len, query);
            }
        }
        if (rest[4] == '/') {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return key_patch(adm, out_status, out_body, out_len, rest + 5, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return key_revoke(adm, out_status, out_body, out_len, rest + 5);
            }
        }
    } else if (strncmp(rest, "models", 6) == 0) {
        if (strcmp(rest, "models") == 0) {
            if (strcmp(method, "POST") == 0) {
                return model_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return model_list(adm, out_status, out_body, out_len, query);
            }
        }
        if (strcmp(rest, "models/sla") == 0 && strcmp(method, "GET") == 0) {
            return admin_models_sla_get(adm, out_status, out_body, out_len);
        }
        if (rest[6] == '/') {
            if (strstr(rest + 7, "/sla/override") != NULL && strcmp(method, "POST") == 0) {
                return admin_model_sla_override(adm, out_status, out_body, out_len, rest + 7, body);
            }
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return model_patch(adm, out_status, out_body, out_len, rest + 7, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return model_delete(adm, out_status, out_body, out_len, rest + 7);
            }
        }
    } else if (strncmp(rest, "groups", 6) == 0) {
        if (strcmp(rest, "groups") == 0) {
            if (strcmp(method, "POST") == 0) {
                return group_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return group_list(adm, out_status, out_body, out_len, query);
            }
        }
        if (rest[6] == '/') {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return group_patch(adm, out_status, out_body, out_len, rest + 7, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return group_delete(adm, out_status, out_body, out_len, rest + 7);
            }
        }
    } else if (strncmp(rest, "providers", 9) == 0) {
        if (strcmp(rest, "providers") == 0) {
            if (strcmp(method, "POST") == 0) {
                return provider_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return provider_list(adm, out_status, out_body, out_len, query);
            }
        }
        if (strcmp(rest, "providers/health") == 0 && strcmp(method, "GET") == 0) {
            return provider_health_get(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "providers/probe") == 0 && strcmp(method, "POST") == 0) {
            return provider_probe_trigger(adm, out_status, out_body, out_len);
        }
        if (rest[9] == '/') {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return provider_patch(adm, out_status, out_body, out_len, rest + 10, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return provider_delete(adm, out_status, out_body, out_len, rest + 10);
            }
            if (strcmp(method, "POST") == 0 && suffix_is_provider_test(rest + 10)) {
                return provider_test(adm, out_status, out_body, out_len, rest + 10);
            }
        }
    } else if (strcmp(rest, "cost") == 0 && strcmp(method, "GET") == 0) {
        return cost_query(adm, out_status, out_body, out_len, query);
    } else if (strcmp(rest, "usage/requests") == 0 && strcmp(method, "GET") == 0) {
        return usage_requests_query(adm, out_status, out_body, out_len, query);
    } else if (strcmp(rest, "usage") == 0 && strcmp(method, "GET") == 0) {
        return usage_query(adm, out_status, out_body, out_len, query);
    } else if (strncmp(rest, "guardrails", 10) == 0) {
        if (strcmp(rest, "guardrails") == 0) {
            if (strcmp(method, "POST") == 0) {
                return guardrails_rule_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return guardrails_rule_list(adm, out_status, out_body, out_len, query);
            }
        }
        if (strcmp(rest, "guardrails/pii") == 0) {
            if (strcmp(method, "GET") == 0) {
                return guardrails_pii_get(adm, out_status, out_body, out_len);
            }
            if (strcmp(method, "PUT") == 0) {
                return guardrails_pii_put(adm, out_status, out_body, out_len, body);
            }
        }
        if (strcmp(rest, "guardrails/pii/test") == 0) {
            if (strcmp(method, "POST") == 0) {
                return guardrails_pii_test(adm, out_status, out_body, out_len, body);
            }
        }
        if (strcmp(rest, "guardrails/reload") == 0) {
            if (strcmp(method, "POST") == 0) {
                return guardrails_reload(adm, out_status, out_body, out_len);
            }
        }
        if (strcmp(rest, "guardrails/webhook/test") == 0) {
            if (strcmp(method, "POST") == 0) {
                return guardrails_webhook_test_handler(adm, out_status, out_body, out_len, body);
            }
        }
        if (rest[10] == '/' && strncmp(rest + 11, "pii", 3) != 0) {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return guardrails_rule_update(adm, out_status, out_body, out_len, rest + 11, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return guardrails_rule_delete(adm, out_status, out_body, out_len, rest + 11);
            }
        }
    } else if (strncmp(rest, "cache/", 6) == 0) {
        if (strcmp(rest, "cache/stats") == 0 && strcmp(method, "GET") == 0) {
            return cache_stats_get(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "cache/purge") == 0 && strcmp(method, "POST") == 0) {
            return cache_purge_trigger(adm, out_status, out_body, out_len, body);
        }
    } else if (strncmp(rest, "traces", 6) == 0) {
        if (strcmp(rest, "traces") == 0 && strcmp(method, "GET") == 0) {
            return admin_traces_list_recent(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "traces/config") == 0) {
            if (strcmp(method, "GET") == 0) {
                return admin_traces_config_get(adm, out_status, out_body, out_len);
            }
            if (strcmp(method, "PUT") == 0) {
                return admin_traces_config_put(adm, out_status, out_body, out_len, body);
            }
        }
        if (rest[6] == '/' && strcmp(method, "GET") == 0) {
            return admin_trace_get_by_id(adm, out_status, out_body, out_len, rest + 7);
        }
    } else if (strncmp(rest, "shadow", 6) == 0) {
        if (strcmp(rest, "shadow/rules") == 0) {
            if (strcmp(method, "GET") == 0) {
                return admin_shadow_rules_list(adm, out_status, out_body, out_len);
            }
            if (strcmp(method, "POST") == 0) {
                return admin_shadow_rules_create(adm, out_status, out_body, out_len, body);
            }
        }
        if (strncmp(rest, "shadow/rules/", 13) == 0) {
            const char* id_str = rest + 13;
            if (strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
                return admin_shadow_rules_update(adm, out_status, out_body, out_len, id_str, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return admin_shadow_rules_delete(adm, out_status, out_body, out_len, id_str);
            }
        }
        if (strcmp(rest, "shadow/evaluations") == 0 && strcmp(method, "GET") == 0) {
            return admin_shadow_evaluations_list(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "shadow/stats") == 0 && strcmp(method, "GET") == 0) {
            return admin_shadow_stats_get(adm, out_status, out_body, out_len);
        }
    } else if (strncmp(rest, "compressor", 10) == 0) {
        if (strcmp(rest, "compressor/rules") == 0) {
            if (strcmp(method, "GET") == 0) {
                return admin_compressor_rules_list(adm, out_status, out_body, out_len);
            }
            if (strcmp(method, "POST") == 0) {
                return admin_compressor_rules_create(adm, out_status, out_body, out_len, body);
            }
        }
        if (strncmp(rest, "compressor/rules/", 17) == 0) {
            const char* id_str = rest + 17;
            if (strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
                return admin_compressor_rules_update(
                    adm, out_status, out_body, out_len, id_str, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return admin_compressor_rules_delete(adm, out_status, out_body, out_len, id_str);
            }
        }
        if (strcmp(rest, "compressor/snapshots") == 0 && strcmp(method, "GET") == 0) {
            return admin_compressor_snapshots_list(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "compressor/stats") == 0 && strcmp(method, "GET") == 0) {
            return admin_compressor_stats_get(adm, out_status, out_body, out_len);
        }
    } else if (strncmp(rest, "cache-optimizer", 15) == 0) {
        if (strcmp(rest, "cache-optimizer/rules") == 0) {
            if (strcmp(method, "GET") == 0) {
                return admin_cache_optimizer_rules_list(adm, out_status, out_body, out_len);
            }
            if (strcmp(method, "POST") == 0) {
                return admin_cache_optimizer_rules_create(adm, out_status, out_body, out_len, body);
            }
        }
        if (strncmp(rest, "cache-optimizer/rules/", 22) == 0) {
            const char* id_str = rest + 22;
            if (strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
                return admin_cache_optimizer_rules_update(
                    adm, out_status, out_body, out_len, id_str, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return admin_cache_optimizer_rules_delete(
                    adm, out_status, out_body, out_len, id_str);
            }
        }
        if (strcmp(rest, "cache-optimizer/snapshots") == 0 && strcmp(method, "GET") == 0) {
            return admin_cache_optimizer_snapshots_list(adm, out_status, out_body, out_len);
        }
        if (strcmp(rest, "cache-optimizer/stats") == 0 && strcmp(method, "GET") == 0) {
            return admin_cache_optimizer_stats_get(adm, out_status, out_body, out_len);
        }
    } else if (strncmp(rest, "audit", 5) == 0) {
        if (strcmp(rest, "audit/events") == 0 && strcmp(method, "GET") == 0) {
            return admin_audit_events_get(adm, out_status, out_body, out_len, query);
        }
        if (strcmp(rest, "audit/violations") == 0 && strcmp(method, "GET") == 0) {
            return admin_audit_violations_get(adm, out_status, out_body, out_len, query);
        }
    }

    return finish_error(out_status, out_body, out_len, 404, "not_found", "no such admin endpoint");
}
