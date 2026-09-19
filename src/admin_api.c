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
#include "model_router.h"
#include "sha256.h"

#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/rand.h>

#define KEY_LIST_CAP 512
#define MODEL_LIST_CAP 256
#define USAGE_LIST_CAP 256

/* ------------------------------------------------------------ helpers */

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
    char            name[128] = "unnamed";
    const char*     n = jstring(jbody, "name", NULL);
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
                return finish_error(status, body, len, 400, "bad_request", "allowed_models entries must be strings");
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

    long id = 0;
    const pg_ops_t* ops = pg_store_ops(adm->ps);
    int rc = ops->create_key(ops->ctx, &k, &id);
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
    int n = 0;
    if (ops->list_keys(ops->ctx, recs, KEY_LIST_CAP, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "key list failed");
    }

    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t*  o = json_object();
        json_t*  al = json_array();
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
key_patch(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest,
          const void* req_body)
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
    int mask = 0;
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
            return finish_error(status, body, len, 400, "bad_request", "allowed_models must be an array");
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
                return finish_error(status, body, len, 400, "bad_request", "allowed_models entries must be strings");
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
            return finish_error(status, body, len, 400, "bad_request", "expires_at must be integer or null");
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
    memcpy(m.name, name, sizeof m.name);
    memcpy(m.provider, jstring(jbody, "provider", "openai"), sizeof m.provider);
    memcpy(m.endpoint, jstring(jbody, "endpoint", ""), sizeof m.endpoint);
    memcpy(m.upstream_key_ref, jstring(jbody, "upstream_key_ref", ""), sizeof m.upstream_key_ref);

    json_t* jparams = json_object_get(jbody, "default_params");
    char    params[1024] = "{}";
    if (jparams != NULL && json_is_object(jparams)) {
        char* packed = json_dumps(jparams, JSON_COMPACT);
        if (packed != NULL) {
            snprintf(params, sizeof params, "%s", packed);
            free(packed);
        }
    }
    memcpy(m.default_params_json, params, sizeof m.default_params_json);
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
    int n = 0;
    if (ops->list_models(ops->ctx, recs, MODEL_LIST_CAP, &n) != 0) {
        free(recs);
        return finish_error(status, body, len, 500, "internal_error", "model list failed");
    }
    json_t* arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t* o = json_object();
        json_object_set_new(o, "model_name", json_string(recs[i].name));
        json_object_set_new(o, "provider", json_string(recs[i].provider));
        json_object_set_new(o, "endpoint", json_string(recs[i].endpoint));
        json_object_set_new(o, "upstream_key_ref", json_string(recs[i].upstream_key_ref));
        json_object_set_new(o, "enabled", json_integer(recs[i].enabled));
        json_object_set_new(o, "default_params", json_string(recs[i].default_params_json));
        json_array_append_new(arr, o);
        model_rec_free(&recs[i]);
    }
    free(recs);
    json_t* root = json_object();
    json_object_set_new(root, "models", arr);
    return finish_json(status, body, len, 200, root);
}

static int
model_patch(admin_ctx_t* adm, int* status, char** body, size_t* len, const char* rest,
            const void* req_body)
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
        if (packed != NULL) {
            snprintf(m.default_params_json, sizeof m.default_params_json, "%s", packed);
            free(packed);
            mask |= MMASK_PARAMS;
        }
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
    size_t flen = strlen(field);
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
    if (key[0] == '\0' || kend == key || key_id <= 0) {
        return finish_error(status, body, len, 400, "bad_request", "?key=<id> is required");
    }
    time_t t_from, t_to;
    if (parse_day(from, &t_from) != 0 || parse_day(to, &t_to) != 0) {
        return finish_error(status, body, len, 400, "bad_request", "bad from/to date (use YYYY-MM-DD)");
    }

    usage_row_t* rows = calloc(USAGE_LIST_CAP, sizeof *rows);
    if (rows == NULL) {
        return -1;
    }
    int n = 0;
    int rc = pg_store_ops(adm->ps)->query_usage(
        pg_store_ops(adm->ps)->ctx, key_id, model[0] != '\0' ? model : NULL, t_from, t_to,
        rows, USAGE_LIST_CAP, &n);
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
admin_dispatch(admin_ctx_t* adm, const char* uri, const char* method, const char* bearer,
               const void* body, size_t body_len, int* out_status, char** out_body, size_t* out_len)
{
    (void)body_len;
    *out_status = 401;
    *out_body = NULL;
    *out_len = 0;

    if (!admin_auth_ok(adm, bearer)) {
        return finish_error(out_status, out_body, out_len, 401, "auth_error", "invalid admin token");
    }

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
            if (strcmp(method, "PATCH") == 0) {
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
            if (strcmp(method, "PATCH") == 0) {
                return model_patch(adm, out_status, out_body, out_len, rest + 7, body);
            }
            if (strcmp(method, "DELETE") == 0) {
                return model_delete(adm, out_status, out_body, out_len, rest + 7);
            }
        }
    } else if (strcmp(rest, "usage") == 0 && strcmp(method, "GET") == 0) {
        return usage_query(adm, out_status, out_body, out_len, query);
    }

    return finish_error(out_status, out_body, out_len, 404, "not_found", "no such admin endpoint");
}
