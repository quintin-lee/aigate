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
#include "circuit_breaker.h"
#include "model_router.h"
#include "secrets.h"
#include "sha256.h"

#include <jansson.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/rand.h>

#define KEY_LIST_CAP 512
#define MODEL_LIST_CAP 256
#define PROVIDER_LIST_CAP 128
#define USAGE_LIST_CAP 256

/* ------------------------------------------------------------ helpers */

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

static int
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

#define LOCKOUT_SLOTS 128

/* Policy is process-wide; transport sets it from env at start-up.
 * Values outside [2..1000] / [5..3600] keep the current value (guards
 * against a misconfig of 0 fails = lock everyone out, or a window so
 * short the guard is useless). */
static int g_lockout_fails = 10;
static int g_lockout_window_s = 300;

typedef struct {
    char           ip[32];
    _Atomic long   fails;
    _Atomic time_t first_fail;
    int            in_use;
} lockout_slot_t;

static lockout_slot_t  g_lockout[LOCKOUT_SLOTS];
static pthread_mutex_t g_lockout_mtx = PTHREAD_MUTEX_INITIALIZER;

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
    lockout_slot_t* s = &g_lockout[lockout_slot_for(ip)];
    pthread_mutex_lock(&g_lockout_mtx);
    int hit = s->in_use && atomic_load(&s->fails) >= g_lockout_fails &&
              time(NULL) - atomic_load(&s->first_fail) < g_lockout_window_s;
    pthread_mutex_unlock(&g_lockout_mtx);
    return hit;
}

static void
lockout_fail(const char* ip)
{
    if (ip == NULL) {
        return;
    }
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

static const char*
jstring(const json_t* obj, const char* field, const char* fallback)
{
    json_t* v = json_object_get(obj, field);
    if (v != NULL && json_is_string(v)) {
        return json_string_value(v);
    }
    return fallback;
}

#define KEY_PLAIN_CAP 48
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
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "key create failed");
    }

    json_t* out = json_object();
    json_object_set_new(out, "key_id", json_integer(id));
    json_object_set_new(out, "plaintext", json_string(plain));
    json_object_set_new(out, "name", json_string(k.name));
    return finish_json(status, body, len, 201, out);
}

static int
key_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    key_rec_t* recs = calloc(KEY_LIST_CAP, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_keys(ops->ctx, recs, KEY_LIST_CAP, &n) != 0) {
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
        json_array_append_new(arr, o);
        key_rec_free(&recs[i]);
    }
    free(recs);

    json_t* root = json_object();
    json_object_set_new(root, "keys", arr);
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
    json_decref(jbody);

    int rc = ops->update_key(ops->ctx, &k, mask);
    key_rec_free(&k);
    if (rc != 0) {
        return finish_error(status, body, len, 500, "internal_error", "key update failed");
    }
    auth_key_invalidate(&adm->ac->keys, existing.key_hash);

    json_t* out = json_object();
    json_object_set_new(out, "key_id", json_integer(id));
    json_object_set_new(out, "updated", json_true());
    return finish_json(status, body, len, 200, out);
}

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
            snprintf(tgt->upstream_key_ref, sizeof tgt->upstream_key_ref, "%s", json_string_value(key));
        }
        json_t* w = json_object_get(item, "weight");
        tgt->weight = (w && json_is_integer(w)) ? (int)json_integer_value(w) : 1;
        if (tgt->weight <= 0) {
            tgt->weight = 1;
        }

        json_t* pr = json_object_get(item, "priority");
        tgt->priority = (pr && json_is_integer(pr)) ? (int)json_integer_value(pr) : 0;
    }
    return 0;
}

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

    json_t* jparams = json_object_get(jbody, "default_params");
    if (jparams != NULL && json_is_object(jparams)) {
        char* packed = json_dumps(jparams, JSON_COMPACT);
        if (packed == NULL) {
            json_decref(jbody);
            return finish_error(status, body, len, 500, "internal_error", "json encode failed");
        }
        if (strlen(packed) >= sizeof m.default_params_json) {
            /* A truncated JSONB blob silently poisons the model row; reject. */
            free(packed);
            json_decref(jbody);
            return finish_error(
                status, body, len, 400, "bad_request", "default_params too large");
        }
        snprintf(m.default_params_json, sizeof m.default_params_json, "%s", packed);
        free(packed);
    } else {
        snprintf(m.default_params_json, sizeof m.default_params_json, "{}");
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

    json_t* out = json_object();
    json_object_set_new(out, "name", json_string(m.name));
    json_object_set_new(out, "created", json_true());
    return finish_json(status, body, len, 201, out);
}

static int
model_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    model_rec_t* recs = calloc(MODEL_LIST_CAP, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_models(ops->ctx, recs, MODEL_LIST_CAP, &n) != 0) {
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

        json_t* tgts_arr = json_array();
        for (int t = 0; t < recs[i].n_targets; t++) {
            upstream_target_t* tgt = &recs[i].targets[t];
            json_t*            to = json_object();
            json_object_set_new(to, "provider", json_string(tgt->provider));
            json_object_set_new(to, "endpoint", json_string(tgt->endpoint));
            json_object_set_new(to, "upstream_key_ref", json_string(tgt->upstream_key_ref));
            json_object_set_new(to, "weight", json_integer(tgt->weight));
            json_object_set_new(to, "priority", json_integer(tgt->priority));

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

        json_array_append_new(arr, o);
        model_rec_free(&recs[i]);
    }
    free(recs);
    json_t* root = json_object();
    json_object_set_new(root, "models", arr);
    return finish_json(status, body, len, 200, root);
}

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
            return finish_error(
                status, body, len, 400, "bad_request", "default_params too large");
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

static int
sync_provider_models(admin_ctx_t* adm, const provider_rec_t* p)
{
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int failed = 0;
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
            } else if (ops->update_model(
                           ops->ctx, &existing, MMASK_ENDPOINT | MMASK_KEYREF | MMASK_ENABLED) !=
                       0) {
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

static int
provider_list(admin_ctx_t* adm, int* status, char** body, size_t* len)
{
    provider_rec_t* recs = calloc(PROVIDER_LIST_CAP, sizeof *recs);
    if (recs == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int             n = 0;
    if (ops->list_providers(ops->ctx, recs, PROVIDER_LIST_CAP, &n) != 0) {
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

    json_t* root = json_object();
    json_object_set_new(root, "providers", arr);
    return finish_json(status, body, len, 200, root);
}

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

static int
usage_query(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* query)
{
    char key[32] = "", model[128] = "", from[16] = "", to[16] = "";
    query_param(query, "key", key, sizeof key);
    query_param(query, "model", model, sizeof model);
    query_param(query, "from", from, sizeof from);
    query_param(query, "to", to, sizeof to);

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

    usage_row_t* rows = calloc(USAGE_LIST_CAP, sizeof *rows);
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
                                                USAGE_LIST_CAP,
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

    json_t* root = json_object();
    json_object_set_new(root, "key_id", json_integer(key_id));
    json_object_set_new(root, "usage", arr);
    return finish_json(status, body, len, 200, root);
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
                return key_list(adm, out_status, out_body, out_len);
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
                return model_list(adm, out_status, out_body, out_len);
            }
        }
        if (rest[6] == '/') {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return model_patch(adm, out_status, out_body, out_len, rest + 7, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return model_delete(adm, out_status, out_body, out_len, rest + 7);
            }
        }
    } else if (strncmp(rest, "providers", 9) == 0) {
        if (strcmp(rest, "providers") == 0) {
            if (strcmp(method, "POST") == 0) {
                return provider_create(adm, out_status, out_body, out_len, body);
            }
            if (strcmp(method, "GET") == 0) {
                return provider_list(adm, out_status, out_body, out_len);
            }
        }
        if (rest[9] == '/') {
            if (strcmp(method, "PATCH") == 0 || strcmp(method, "PUT") == 0) {
                return provider_patch(adm, out_status, out_body, out_len, rest + 10, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return provider_delete(adm, out_status, out_body, out_len, rest + 10);
            }
        }
    } else if (strcmp(rest, "usage") == 0 && strcmp(method, "GET") == 0) {
        return usage_query(adm, out_status, out_body, out_len, query);
    }

    return finish_error(out_status, out_body, out_len, 404, "not_found", "no such admin endpoint");
}
