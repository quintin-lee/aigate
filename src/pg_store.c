/** @file pg_store.c
 *  @brief PostgreSQL persistence: real libpq ops + schema migration runner.
 *
 *  The libpq backend owns one connection guarded by a mutex; admin
 *  mutations and the flush worker share it. Hot-path request checks never
 *  touch this file (they go through the ops table; unit tests supply
 *  in-memory fakes).
 *
 *  All SQL uses PQexecParams with string parameters (text protocol);
 *  NULL semantics are expressed with the "null" sentinel string and
 *  CASE expressions, which keeps the parameter marshaling trivial.
 */
#include "pg_store.h"
#include "aigate_log.h"
#include "schema_sql.h"

#include <jansson.h>
#include <libpq-fe.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pg_store {
    pg_ops_t ops; /* live ops table (libpq or fake) */
    void*    ctx; /* pq_ctx when owns_ctx, else caller-provided */
    int      owns_ctx;
};

struct pq_ctx {
    PGconn*         db;
    char            dsn[1024];
    pthread_mutex_t mtx;
};

/* ------------------------------------------------------------ helpers */

void
key_rec_free(key_rec_t* k)
{
    if (k == NULL) {
        return;
    }
    for (int i = 0; i < k->n_allowed; i++) {
        free(k->allowed_models[i]);
    }
    free(k->allowed_models);
    k->allowed_models = NULL;
    k->n_allowed = 0;
}

void
model_rec_free(model_rec_t* m)
{
    (void)m; /* fixed-size records; nothing dynamic to free */
}

void
provider_rec_free(provider_rec_t* p)
{
    if (p == NULL) {
        return;
    }
    if (p->models != NULL) {
        for (int i = 0; i < p->n_models; i++) {
            free(p->models[i]);
        }
        free(p->models);
        p->models = NULL;
    }
    p->n_models = 0;
}

static int
join_provider_models(const provider_rec_t* p, char* buf, size_t cap)
{
    int off = 0;
    for (int i = 0; i < p->n_models; i++) {
        int w = snprintf(buf + off, cap - (size_t)off, "%s%s", i > 0 ? "|" : "", p->models[i]);
        if (w < 0 || (size_t)w >= cap - (size_t)off) {
            return -1;
        }
        off += w;
    }
    if (p->n_models == 0 && cap > 0) {
        buf[0] = '\0';
    }
    return 0;
}

static void
copy_field(char* dst, size_t cap, const char* src)
{
    snprintf(dst, cap, "%s", src == NULL ? "" : src);
}

/** @brief Format a UTC day (midnight) as "YYYY-MM-DD".
 * @note Out-of-range years are clamped to 1970-01-01 to keep %04d safe. */
static void
fmt_day(time_t day, char out[24])
{
    struct tm tmv;
    int       y, m, d;
    if (gmtime_r(&day, &tmv) == NULL) {
        y = 1970, m = 1, d = 1;
    } else {
        y = tmv.tm_year + 1900;
        m = tmv.tm_mon + 1;
        d = tmv.tm_mday;
    }
    if (y < 1000 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) {
        y = 1970, m = 1, d = 1;
    }
    snprintf(out, 24, "%04d-%02d-%02d", y, m, d);
}

/** @brief Parse "YYYY-MM-DD" (UTC) to a midnight-UTC time_t. */
static time_t
parse_day(const char* s)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = atoi(s) - 1900;
    tmv.tm_mon = atoi(s + 5) - 1;
    tmv.tm_mday = atoi(s + 8);
    return timegm(&tmv);
}

/** @brief Parse a "|"-joined model list; empty string means allow-all.
 * @return 0 ok, -1 allocation failure (out params reset). */
static int
parse_model_list(const char* joined, char*** out_vec, int* out_n)
{
    *out_vec = NULL;
    *out_n = 0;
    if (joined == NULL) {
        return 0;
    }
    int n = 1;
    for (const char* p = joined; *p; p++) {
        if (*p == '|') {
            n++;
        }
    }
    char** v = malloc(sizeof *v * (size_t)n);
    if (v == NULL) {
        return -1;
    }
    int         idx = 0;
    const char* start = joined;
    for (const char* p = joined;; p++) {
        if (*p == '|' || *p == '\0') {
            char* tok = malloc((size_t)(p - start) + 1);
            if (tok == NULL) {
                for (int i = 0; i < idx; i++) {
                    free(v[i]);
                }
                free(v);
                return -1;
            }
            memcpy(tok, start, (size_t)(p - start));
            tok[p - start] = '\0';
            if (p - start > 0) {
                v[idx++] = tok;
            } else {
                free(tok);
            }
            if (*p == '\0') {
                break;
            }
            start = p + 1;
        }
    }
    *out_vec = v;
    *out_n = idx;
    return 0;
}

/** @brief Join a key's allowlist to a "|"-string ("" when empty). */
static int
join_model_list(const key_rec_t* k, char* buf, size_t cap)
{
    int off = 0;
    for (int i = 0; i < k->n_allowed; i++) {
        int w =
            snprintf(buf + off, cap - (size_t)off, "%s%s", i > 0 ? "|" : "", k->allowed_models[i]);
        if (w < 0 || (size_t)w >= cap - (size_t)off) {
            return -1;
        }
        off += w;
    }
    return 0;
}

/* ------------------------------------------------------------ libpq ops */

static void
pq_ensure_conn(struct pq_ctx* px)
{
    if (PQstatus(px->db) == CONNECTION_OK) {
        return;
    }
    AIGATE_LOG_WARN("pg: connection lost, reconnecting");
    PQfinish(px->db);
    px->db = PQconnectdb(px->dsn);
    if (PQstatus(px->db) != CONNECTION_OK) {
        AIGATE_LOG_ERROR("pg: reconnect failed: %s", PQerrorMessage(px->db));
    }
}

static void
pq_lock(struct pq_ctx* px)
{
    pthread_mutex_lock(&px->mtx);
    pq_ensure_conn(px);
}

static void
pq_unlock(struct pq_ctx* px)
{
    pthread_mutex_unlock(&px->mtx);
}

static int
pq_get_key_by_hash(void* vctx, const char* key_hash, key_rec_t* out)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "SELECT key_id, key_hash, name, array_to_string(allowed_models, '|'), "
                            "rate_qps, daily_token_quota, expires_at, revoked_at "
                            "FROM api_keys WHERE key_hash = $1";
    const char*       val[1] = {key_hash};
    int               plen[1] = {0};
    int               rc = -1;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, val, plen, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res != NULL) {
            AIGATE_LOG_ERROR("pg get_key_by_hash: %s", PQerrorMessage(px->db));
        }
        PQclear(res);
        return -1;
    }
    if (PQntuples(res) > 0) {
        memset(out, 0, sizeof *out);
        out->key_id = atol(PQgetvalue(res, 0, 0));
        copy_field(out->key_hash, sizeof out->key_hash, PQgetvalue(res, 0, 1));
        copy_field(out->name, sizeof out->name, PQgetvalue(res, 0, 2));
        if (parse_model_list(PQgetvalue(res, 0, 3), &out->allowed_models, &out->n_allowed) == 0) {
            out->rate_qps = atoi(PQgetvalue(res, 0, 4));
            out->daily_token_quota = atol(PQgetvalue(res, 0, 5));
            const char* exp = PQgetvalue(res, 0, 6);
            const char* rev = PQgetvalue(res, 0, 7);
            if (exp != NULL && exp[0] != '\0') {
                out->expires_at = (time_t)atol(exp);
                out->has_expiry = 1;
            }
            out->revoked = (rev != NULL && rev[0] != '\0');
            rc = 0;
        } else {
            key_rec_free(out);
        }
    } else {
        rc = 1; /* query succeeded but no such key */
    }
    PQclear(res);
    return rc; /* 0 found, 1 missing, -1 error */
}

/** @brief Fill one api_keys row into @p out (allowlist deep-copied). */
static void
fill_key_row(PGresult* res, int row, key_rec_t* out)
{
    memset(out, 0, sizeof *out);
    out->key_id = atol(PQgetvalue(res, row, 0));
    copy_field(out->key_hash, sizeof out->key_hash, PQgetvalue(res, row, 1));
    copy_field(out->name, sizeof out->name, PQgetvalue(res, row, 2));
    if (parse_model_list(PQgetvalue(res, row, 3), &out->allowed_models, &out->n_allowed) != 0) {
        key_rec_free(out);
        return;
    }
    out->rate_qps = atoi(PQgetvalue(res, row, 4));
    out->daily_token_quota = atol(PQgetvalue(res, row, 5));
    const char* exp = PQgetvalue(res, row, 6);
    const char* rev = PQgetvalue(res, row, 7);
    if (exp != NULL && exp[0] != '\0') {
        out->expires_at = (time_t)atol(exp);
        out->has_expiry = 1;
    }
    out->revoked = (rev != NULL && rev[0] != '\0');
}

static int
pq_list_keys(void* vctx, key_rec_t* out, int cap, int* n)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "SELECT key_id, key_hash, name, "
                            "array_to_string(allowed_models, '|'), rate_qps, daily_token_quota, "
                            "expires_at, revoked_at FROM api_keys ORDER BY key_id";
    *n = 0;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 0, NULL, NULL, NULL, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        AIGATE_LOG_ERROR("pg list_keys: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    int nt = PQntuples(res);
    if (nt > cap) {
        nt = cap;
    }
    for (int i = 0; i < nt; i++) {
        fill_key_row(res, i, &out[i]);
    }
    *n = nt;
    PQclear(res);
    return 0;
}

static int
pq_get_key_by_id(void* vctx, long key_id, key_rec_t* out)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "SELECT key_id, key_hash, name, "
                            "array_to_string(allowed_models, '|'), rate_qps, daily_token_quota, "
                            "expires_at, revoked_at FROM api_keys WHERE key_id = $1";
    char              id[32];
    const char*       val[1] = {0};
    int               plen[1] = {0};
    int               rc = -1;

    snprintf(id, sizeof id, "%ld", key_id);
    val[0] = id;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, val, plen, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res != NULL) {
            AIGATE_LOG_ERROR("pg get_key_by_id: %s", PQerrorMessage(px->db));
        }
        PQclear(res);
        return -1;
    }
    if (PQntuples(res) > 0) {
        fill_key_row(res, 0, out);
        rc = 0;
    }
    PQclear(res);
    return rc; /* 0 found, -1 unknown/error */
}

static int
fill_model_row(PGresult* res, int row, model_rec_t* out)
{
    memset(out, 0, sizeof *out);
    copy_field(out->name, sizeof out->name, PQgetvalue(res, row, 0));
    copy_field(out->provider, sizeof out->provider, PQgetvalue(res, row, 1));
    copy_field(out->endpoint, sizeof out->endpoint, PQgetvalue(res, row, 2));
    copy_field(out->upstream_key_ref, sizeof out->upstream_key_ref, PQgetvalue(res, row, 3));
    copy_field(out->default_params_json, sizeof out->default_params_json, PQgetvalue(res, row, 4));
    out->enabled = strcmp(PQgetvalue(res, row, 5), "t") == 0;

    int nfields = PQnfields(res);
    if (nfields > 6) {
        const char* t_raw = PQgetvalue(res, row, 6);
        if (t_raw != NULL && t_raw[0] != '\0' && strcmp(t_raw, "[]") != 0) {
            json_t* jarr = json_loads(t_raw, 0, NULL);
            if (jarr != NULL && json_is_array(jarr)) {
                size_t  idx;
                json_t* item;
                json_array_foreach(jarr, idx, item)
                {
                    if (out->n_targets >= MAX_TARGETS_PER_MODEL) {
                        break;
                    }
                    if (!json_is_object(item)) {
                        continue;
                    }
                    upstream_target_t* tgt = &out->targets[out->n_targets++];
                    memset(tgt, 0, sizeof *tgt);
                    json_t* jp = json_object_get(item, "provider");
                    json_t* je = json_object_get(item, "endpoint");
                    json_t* jk = json_object_get(item, "upstream_key_ref");
                    json_t* jw = json_object_get(item, "weight");
                    json_t* jpr = json_object_get(item, "priority");

                    copy_field(tgt->provider,
                               sizeof tgt->provider,
                               (jp && json_is_string(jp)) ? json_string_value(jp) : out->provider);
                    copy_field(tgt->endpoint,
                               sizeof tgt->endpoint,
                               (je && json_is_string(je)) ? json_string_value(je) : out->endpoint);
                    copy_field(tgt->upstream_key_ref,
                               sizeof tgt->upstream_key_ref,
                               (jk && json_is_string(jk)) ? json_string_value(jk) : "");
                    tgt->weight = (jw && json_is_integer(jw) && json_integer_value(jw) > 0)
                                      ? (int)json_integer_value(jw)
                                      : 1;
                    tgt->priority =
                        (jpr && json_is_integer(jpr)) ? (int)json_integer_value(jpr) : 0;
                }
                json_decref(jarr);
            }
        }
    }
    if (nfields > 7) {
        copy_field(out->lb_policy, sizeof out->lb_policy, PQgetvalue(res, row, 7));
    }
    if (out->lb_policy[0] == '\0') {
        snprintf(out->lb_policy, sizeof out->lb_policy, "priority");
    }

    /* Fallback: if no targets configured, synthesize targets[0] from primary fields */
    if (out->n_targets == 0) {
        out->n_targets = 1;
        snprintf(out->targets[0].provider, sizeof out->targets[0].provider, "%s", out->provider);
        snprintf(out->targets[0].endpoint, sizeof out->targets[0].endpoint, "%s", out->endpoint);
        snprintf(out->targets[0].upstream_key_ref,
                 sizeof out->targets[0].upstream_key_ref,
                 "%s",
                 out->upstream_key_ref);
        out->targets[0].weight = 1;
        out->targets[0].priority = 0;
    }

    return 0;
}

static int
pq_get_model(void* vctx, const char* name, model_rec_t* out)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "SELECT model_name, provider, endpoint, COALESCE(upstream_key_ref, ''), "
        "default_params::text, enabled, COALESCE(targets::text, '[]'), COALESCE(lb_policy, "
        "'priority') "
        "FROM models WHERE model_name = $1 AND enabled = true";
    const char* val[1] = {name};
    int         plen[1] = {0};
    int         rc = -1;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, val, plen, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res != NULL) {
            AIGATE_LOG_ERROR("pg get_model: %s", PQerrorMessage(px->db));
        }
        PQclear(res);
        return -1;
    }
    if (PQntuples(res) > 0) {
        fill_model_row(res, 0, out);
        rc = 0;
    }
    PQclear(res);
    return rc;
}

static int
pq_list_models(void* vctx, model_rec_t* out, int cap, int* n)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "SELECT model_name, provider, endpoint, COALESCE(upstream_key_ref, ''), "
        "default_params::text, enabled, COALESCE(targets::text, '[]'), COALESCE(lb_policy, "
        "'priority') "
        "FROM models ORDER BY model_name";
    *n = 0;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 0, NULL, NULL, NULL, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        AIGATE_LOG_ERROR("pg list_models: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    int nt = PQntuples(res);
    if (nt > cap) {
        nt = cap;
    }
    for (int i = 0; i < nt; i++) {
        fill_model_row(res, i, &out[i]);
    }
    *n = nt;
    PQclear(res);
    return 0;
}

static int
pq_create_key(void* vctx, const key_rec_t* k, long* out_key_id)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "INSERT INTO api_keys(key_hash, name, allowed_models, rate_qps, "
        "daily_token_quota, expires_at) "
        "VALUES($1, $2, CASE WHEN $3 = '' THEN '{}'::text[] "
        "ELSE string_to_array($3, '|') END, $4, $5, "
        "CASE WHEN $6 = 'null' THEN NULL "
        "ELSE to_timestamp(($6)::double precision)::timestamp with time zone END) RETURNING key_id";
    char        joined[512], rate[16], quota[32], exp[32];
    const char* vals[6];
    int         plens[6] = {0};
    long        id = -1;

    if (join_model_list(k, joined, sizeof joined) != 0) {
        return -1;
    }
    snprintf(rate, sizeof rate, "%d", k->rate_qps);
    snprintf(quota, sizeof quota, "%ld", k->daily_token_quota);
    if (k->has_expiry) {
        snprintf(exp, sizeof exp, "%ld", (long)k->expires_at);
    } else {
        strcpy(exp, "null");
    }
    vals[0] = k->key_hash;
    vals[1] = k->name;
    vals[2] = joined;
    vals[3] = rate;
    vals[4] = quota;
    vals[5] = exp;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 6, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        id = atol(PQgetvalue(res, 0, 0));
    } else {
        AIGATE_LOG_ERROR("pg create_key: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    if (id < 0) {
        return -1;
    }
    *out_key_id = id;
    return 0;
}

static int
pq_update_key(void* vctx, const key_rec_t* k, int mask)
{
    struct pq_ctx* px = vctx;
    char           sql[1024], joined[512], rate[16], quota[32], exp[32], id[32];
    const char*    vals[5];
    int            plens[5] = {0};
    int            nv = 0, off;

    if (mask == 0) {
        return 0;
    }
    if (join_model_list(k, joined, sizeof joined) != 0) {
        return -1;
    }
    snprintf(rate, sizeof rate, "%d", k->rate_qps);
    snprintf(quota, sizeof quota, "%ld", k->daily_token_quota);
    if (k->has_expiry) {
        snprintf(exp, sizeof exp, "%ld", (long)k->expires_at);
    } else {
        strcpy(exp, "null");
    }
    snprintf(id, sizeof id, "%ld", k->key_id);

    off = snprintf(sql, sizeof sql, "UPDATE api_keys SET ");
    if (mask & KMASK_RATE) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, "rate_qps = $%d", nv);
        vals[nv - 1] = rate;
    }
    if (mask & KMASK_QUOTA) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        "%sdaily_token_quota = $%d",
                        nv > 1 ? ", " : "",
                        nv);
        vals[nv - 1] = quota;
    }
    if (mask & KMASK_ALLOWLIST) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        "%sallowed_models = CASE WHEN $%d = '' THEN '{}'::text[] "
                        "ELSE string_to_array($%d, '|') END",
                        nv > 1 ? ", " : "",
                        nv,
                        nv);
        vals[nv - 1] = joined;
    }
    if (mask & KMASK_EXPIRY) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        "%sexpires_at = CASE WHEN $%d = 'null' THEN NULL "
                        "ELSE to_timestamp(($%d)::double precision)::timestamp with time zone END",
                        nv > 1 ? ", " : "",
                        nv,
                        nv);
        vals[nv - 1] = exp;
    }
    nv++;
    off += snprintf(sql + off, sizeof sql - (size_t)off, " WHERE key_id = $%d", nv);
    vals[nv - 1] = id;

    int ok = 0;
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, sql, nv, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        ok = 1;
    } else {
        AIGATE_LOG_ERROR("pg update_key: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return ok ? 0 : -1;
}

static int
pq_revoke_key(void* vctx, long key_id)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "UPDATE api_keys SET revoked_at = now() "
                            "WHERE key_id = $1 AND revoked_at IS NULL";
    char              id[32];
    const char*       vals[1];
    int               plens[1] = {0};
    int               n = 0;

    snprintf(id, sizeof id, "%ld", key_id);
    vals[0] = id;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        n = atoi(PQcmdTuples(res));
    } else {
        AIGATE_LOG_ERROR("pg revoke_key: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return n > 0 ? 0 : -1;
}

static char*
serialize_targets_json(const model_rec_t* m)
{
    json_t* jarr = json_array();
    if (jarr == NULL) {
        return NULL;
    }
    if (m->n_targets == 0 && m->endpoint[0] != '\0') {
        json_t* item = json_pack("{s:s, s:s, s:s, s:i, s:i}",
                                 "provider",
                                 m->provider[0] != '\0' ? m->provider : "openai",
                                 "endpoint",
                                 m->endpoint,
                                 "upstream_key_ref",
                                 m->upstream_key_ref,
                                 "weight",
                                 1,
                                 "priority",
                                 0);
        if (item != NULL) {
            json_array_append_new(jarr, item);
        }
    } else {
        for (int i = 0; i < m->n_targets && i < MAX_TARGETS_PER_MODEL; i++) {
            const upstream_target_t* tgt = &m->targets[i];
            json_t* item = json_pack("{s:s, s:s, s:s, s:i, s:i}",
                                     "provider",
                                     tgt->provider[0] != '\0' ? tgt->provider : "openai",
                                     "endpoint",
                                     tgt->endpoint,
                                     "upstream_key_ref",
                                     tgt->upstream_key_ref,
                                     "weight",
                                     tgt->weight > 0 ? tgt->weight : 1,
                                     "priority",
                                     tgt->priority >= 0 ? tgt->priority : 0);
            if (item != NULL) {
                json_array_append_new(jarr, item);
            }
        }
    }
    char* s = json_dumps(jarr, JSON_COMPACT);
    json_decref(jarr);
    return s;
}

static int
pq_create_model(void* vctx, const model_rec_t* m)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "INSERT INTO models(model_name, provider, endpoint, upstream_key_ref, "
        "default_params, targets, lb_policy) "
        "VALUES($1, $2, $3, CASE WHEN $4 = '' THEN NULL ELSE $4 END, $5::jsonb, $6::jsonb, $7)";
    const char* vals[7];
    int         plens[7] = {0};

    char*       targets_json = serialize_targets_json(m);
    const char* t_str = targets_json ? targets_json : "[]";
    const char* lb = (m->lb_policy[0] != '\0') ? m->lb_policy : "priority";
    const char* prov = (m->provider[0] != '\0')
                           ? m->provider
                           : (m->n_targets > 0 ? m->targets[0].provider : "openai");
    const char* ep =
        (m->endpoint[0] != '\0') ? m->endpoint : (m->n_targets > 0 ? m->targets[0].endpoint : "");
    const char* kr = (m->upstream_key_ref[0] != '\0')
                         ? m->upstream_key_ref
                         : (m->n_targets > 0 ? m->targets[0].upstream_key_ref : "");

    vals[0] = m->name;
    vals[1] = prov;
    vals[2] = ep;
    vals[3] = kr;
    vals[4] = m->default_params_json[0] != '\0' ? m->default_params_json : "{}";
    vals[5] = t_str;
    vals[6] = lb;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 7, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (targets_json != NULL) {
        free(targets_json);
    }
    if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
        AIGATE_LOG_ERROR("pg create_model: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

static int
pq_update_model(void* vctx, const model_rec_t* m, int mask)
{
    struct pq_ctx* px = vctx;
    char           sql[2048];
    const char*    vals[8];
    int            plens[8] = {0};
    int            nv = 0, off;
    char*          targets_json = NULL;

    if (mask == 0) {
        return 0;
    }

    off = snprintf(sql, sizeof sql, "UPDATE models SET ");
    if (mask & MMASK_ENDPOINT) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, "endpoint = $%d", nv);
        vals[nv - 1] = m->endpoint;
    }
    if (mask & MMASK_PARAMS) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        "%sdefault_params = $%d::jsonb",
                        nv > 1 ? ", " : "",
                        nv);
        vals[nv - 1] = m->default_params_json;
    }
    if (mask & MMASK_KEYREF) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        "%supstream_key_ref = CASE WHEN $%d = '' THEN NULL "
                        "ELSE $%d END",
                        nv > 1 ? ", " : "",
                        nv,
                        nv);
        vals[nv - 1] = m->upstream_key_ref;
    }
    if (mask & MMASK_ENABLED) {
        nv++;
        off += snprintf(
            sql + off, sizeof sql - (size_t)off, "%senabled = $%d", nv > 1 ? ", " : "", nv);
        vals[nv - 1] = m->enabled ? "true" : "false";
    }
    if (mask & MMASK_TARGETS) {
        targets_json = serialize_targets_json(m);
        nv++;
        off += snprintf(
            sql + off, sizeof sql - (size_t)off, "%stargets = $%d::jsonb", nv > 1 ? ", " : "", nv);
        vals[nv - 1] = targets_json ? targets_json : "[]";
    }
    if (mask & MMASK_LB_POLICY) {
        nv++;
        off += snprintf(
            sql + off, sizeof sql - (size_t)off, "%slb_policy = $%d", nv > 1 ? ", " : "", nv);
        vals[nv - 1] = m->lb_policy[0] != '\0' ? m->lb_policy : "priority";
    }
    nv++;
    off += snprintf(sql + off, sizeof sql - (size_t)off, " WHERE model_name = $%d", nv);
    vals[nv - 1] = m->name;

    int ok = 0;
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, sql, nv, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (targets_json != NULL) {
        free(targets_json);
    }
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        ok = 1;
    } else {
        AIGATE_LOG_ERROR("pg update_model: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return ok ? 0 : -1;
}

static int
pq_delete_model(void* vctx, const char* name)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "DELETE FROM models WHERE model_name = $1";
    const char*       vals[1] = {name};
    int               plens[1] = {0};
    int               n = 0;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        n = atoi(PQcmdTuples(res));
    } else {
        AIGATE_LOG_ERROR("pg delete_model: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return n > 0 ? 0 : -1;
}

static int
fill_provider_row(PGresult* res, int row, provider_rec_t* out)
{
    memset(out, 0, sizeof *out);
    out->id = atol(PQgetvalue(res, row, 0));
    copy_field(out->name, sizeof out->name, PQgetvalue(res, row, 1));
    copy_field(out->provider_type, sizeof out->provider_type, PQgetvalue(res, row, 2));
    copy_field(out->endpoint, sizeof out->endpoint, PQgetvalue(res, row, 3));
    copy_field(out->api_key, sizeof out->api_key, PQgetvalue(res, row, 4));
    if (parse_model_list(PQgetvalue(res, row, 5), &out->models, &out->n_models) != 0) {
        provider_rec_free(out);
        return -1;
    }
    out->enabled = strcmp(PQgetvalue(res, row, 6), "t") == 0;
    const char* cat = PQgetvalue(res, row, 7);
    if (cat != NULL && cat[0] != '\0') {
        out->created_at = (time_t)atol(cat);
    }
    return 0;
}

static int
pq_list_providers(void* vctx, provider_rec_t* out, int cap, int* n)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "SELECT id, name, provider_type, endpoint, COALESCE(api_key, ''), "
        "array_to_string(models, '|'), enabled, EXTRACT(EPOCH FROM created_at)::bigint "
        "FROM providers ORDER BY id";
    *n = 0;

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 0, NULL, NULL, NULL, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        AIGATE_LOG_ERROR("pg list_providers: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    int nt = PQntuples(res);
    if (nt > cap) {
        nt = cap;
    }
    for (int i = 0; i < nt; i++) {
        fill_provider_row(res, i, &out[i]);
    }
    *n = nt;
    PQclear(res);
    return 0;
}

static int
pq_get_provider(void* vctx, long id, provider_rec_t* out)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "SELECT id, name, provider_type, endpoint, COALESCE(api_key, ''), "
        "array_to_string(models, '|'), enabled, EXTRACT(EPOCH FROM created_at)::bigint "
        "FROM providers WHERE id = $1";
    char        id_str[32];
    const char* val[1] = {id_str};
    int         plen[1] = {0};
    int         rc = -1;

    snprintf(id_str, sizeof id_str, "%ld", id);
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, val, plen, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res != NULL) {
            AIGATE_LOG_ERROR("pg get_provider: %s", PQerrorMessage(px->db));
        }
        PQclear(res);
        return -1;
    }
    if (PQntuples(res) > 0) {
        fill_provider_row(res, 0, out);
        rc = 0;
    }
    PQclear(res);
    return rc;
}

static int
pq_create_provider(void* vctx, const provider_rec_t* p, long* out_id)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "INSERT INTO providers(name, provider_type, endpoint, api_key, models, enabled) "
        "VALUES($1, $2, $3, $4, "
        "CASE WHEN $5 = '' THEN '{}'::text[] ELSE string_to_array($5, '|') END, $6::boolean) "
        "RETURNING id";
    char        joined[4096];
    const char* vals[6];
    int         plens[6] = {0};
    long        id = -1;

    if (join_provider_models(p, joined, sizeof joined) != 0) {
        return -1;
    }
    vals[0] = p->name;
    vals[1] = p->provider_type;
    vals[2] = p->endpoint;
    vals[3] = p->api_key;
    vals[4] = joined;
    vals[5] = p->enabled ? "true" : "false";

    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 6, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        id = atol(PQgetvalue(res, 0, 0));
    } else {
        AIGATE_LOG_ERROR("pg create_provider: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    if (id < 0) {
        return -1;
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return 0;
}

static int
pq_update_provider(void* vctx, const provider_rec_t* p, int mask)
{
    struct pq_ctx* px = vctx;
    char           sql[2048], joined[4096], id[32];
    const char*    vals[7];
    int            plens[7] = {0};
    int            nv = 0, off;

    if (mask == 0) {
        return 0;
    }
    if (mask & PMASK_MODELS) {
        if (join_provider_models(p, joined, sizeof joined) != 0) {
            return -1;
        }
    }
    snprintf(id, sizeof id, "%ld", p->id);

    off = snprintf(sql, sizeof sql, "UPDATE providers SET updated_at = now()");
    if (mask & PMASK_TYPE) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, ", provider_type = $%d", nv);
        vals[nv - 1] = p->provider_type;
    }
    if (mask & PMASK_ENDPOINT) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, ", endpoint = $%d", nv);
        vals[nv - 1] = p->endpoint;
    }
    if (mask & PMASK_API_KEY) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, ", api_key = $%d", nv);
        vals[nv - 1] = p->api_key;
    }
    if (mask & PMASK_MODELS) {
        nv++;
        off += snprintf(sql + off,
                        sizeof sql - (size_t)off,
                        ", models = CASE WHEN $%d = '' THEN '{}'::text[] "
                        "ELSE string_to_array($%d, '|') END",
                        nv,
                        nv);
        vals[nv - 1] = joined;
    }
    if (mask & PMASK_ENABLED) {
        nv++;
        off += snprintf(sql + off, sizeof sql - (size_t)off, ", enabled = $%d::boolean", nv);
        vals[nv - 1] = p->enabled ? "true" : "false";
    }
    nv++;
    off += snprintf(sql + off, sizeof sql - (size_t)off, " WHERE id = $%d", nv);
    vals[nv - 1] = id;

    int ok = 0;
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, sql, nv, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        ok = 1;
    } else {
        AIGATE_LOG_ERROR("pg update_provider: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return ok ? 0 : -1;
}

static int
pq_delete_provider(void* vctx, long id)
{
    struct pq_ctx*    px = vctx;
    static const char q[] = "DELETE FROM providers WHERE id = $1";
    char              id_str[32];
    const char*       vals[1] = {id_str};
    int               plens[1] = {0};
    int               n = 0;

    snprintf(id_str, sizeof id_str, "%ld", id);
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 1, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res != NULL && PQresultStatus(res) == PGRES_COMMAND_OK) {
        n = atoi(PQcmdTuples(res));
    } else {
        AIGATE_LOG_ERROR("pg delete_provider: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
    }
    PQclear(res);
    return n > 0 ? 0 : -1;
}

static int
pq_flush_usage(void* vctx, const usage_row_t* rows, int n)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "INSERT INTO usage_daily(key_id, model_name, day, requests, prompt_tokens, "
        "completion_tokens, errors, cached_prompt_tokens) "
        "VALUES($1, $2, to_date($3, 'YYYY-MM-DD'), $4, $5, $6, $7, $8) "
        "ON CONFLICT (key_id, model_name, day) DO UPDATE SET "
        "requests = usage_daily.requests + EXCLUDED.requests, "
        "prompt_tokens = usage_daily.prompt_tokens + EXCLUDED.prompt_tokens, "
        "completion_tokens = usage_daily.completion_tokens + EXCLUDED.completion_tokens, "
        "errors = usage_daily.errors + EXCLUDED.errors, "
        "cached_prompt_tokens = usage_daily.cached_prompt_tokens + EXCLUDED.cached_prompt_tokens";
    int rc = 0;

    if (n <= 0) {
        return 0;
    }

    char        day[24], num[32], n_req[32], n_ptok[32], n_ctok[32], n_err[32], n_cptok[32];
    const char* vals[8];
    int         plens[8] = {0};

    pq_lock(px);
    PQclear(PQexec(px->db, "BEGIN"));
    for (int i = 0; i < n; i++) {
        fmt_day(rows[i].day, day);
        snprintf(num, sizeof num, "%ld", rows[i].key_id);
        vals[0] = num;
        vals[1] = rows[i].model_name;
        vals[2] = day;
        snprintf(n_req, sizeof n_req, "%ld", rows[i].requests);
        vals[3] = n_req;
        snprintf(n_ptok, sizeof n_ptok, "%ld", rows[i].prompt_tokens);
        vals[4] = n_ptok;
        snprintf(n_ctok, sizeof n_ctok, "%ld", rows[i].completion_tokens);
        vals[5] = n_ctok;
        snprintf(n_err, sizeof n_err, "%ld", rows[i].errors);
        vals[6] = n_err;
        snprintf(n_cptok, sizeof n_cptok, "%ld", rows[i].cached_prompt_tokens);
        vals[7] = n_cptok;
        PGresult* res = PQexecParams(px->db, q, 8, NULL, vals, plens, NULL, 0);
        if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
            AIGATE_LOG_ERROR("pg flush_usage: %s",
                             res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
            PQclear(res);
            PQclear(PQexec(px->db, "ROLLBACK"));
            rc = -1;
            break;
        }
        PQclear(res);
    }
    PQclear(PQexec(px->db, "COMMIT"));
    pq_unlock(px);
    return rc;
}

static int
pq_query_usage(void*        vctx,
               long         key_id,
               const char*  model,
               time_t       from,
               time_t       to,
               usage_row_t* out,
               int          cap,
               int*         n)
{
    struct pq_ctx*    px = vctx;
    static const char q[] =
        "SELECT key_id, model_name, day, requests, prompt_tokens, "
        "completion_tokens, errors, cached_prompt_tokens FROM usage_daily "
        "WHERE ($1 = '0' OR key_id = $1::bigint) AND ($2 = 'all' OR model_name = $2) "
        "AND day >= to_date($3, 'YYYY-MM-DD') AND day <= to_date($4, 'YYYY-MM-DD') "
        "ORDER BY day";
    char        key[32], dfrom[24], dto[24];
    const char* vals[4];
    int         plens[4] = {0};

    fmt_day(from, dfrom);
    fmt_day(to, dto);
    snprintf(key, sizeof key, "%ld", key_id);
    vals[0] = key;
    vals[1] = (model == NULL || model[0] == '\0') ? "all" : model;
    vals[2] = dfrom;
    vals[3] = dto;

    *n = 0;
    pq_lock(px);
    PGresult* res = PQexecParams(px->db, q, 4, NULL, vals, plens, NULL, 0);
    pq_unlock(px);
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        AIGATE_LOG_ERROR("pg query_usage: %s",
                         res != NULL ? PQerrorMessage(px->db) : "query alloc failed");
        PQclear(res);
        return -1;
    }
    int nt = PQntuples(res);
    if (nt > cap) {
        nt = cap;
    }
    for (int i = 0; i < nt; i++) {
        out[i].key_id = atol(PQgetvalue(res, i, 0));
        copy_field(out[i].model_name, sizeof out[i].model_name, PQgetvalue(res, i, 1));
        out[i].day = parse_day(PQgetvalue(res, i, 2));
        out[i].requests = atol(PQgetvalue(res, i, 3));
        out[i].prompt_tokens = atol(PQgetvalue(res, i, 4));
        out[i].completion_tokens = atol(PQgetvalue(res, i, 5));
        out[i].errors = atol(PQgetvalue(res, i, 6));
        out[i].cached_prompt_tokens = PQnfields(res) > 7 ? atol(PQgetvalue(res, i, 7)) : 0;
    }
    *n = nt;
    PQclear(res);
    return 0;
}

/* ------------------------------------------------------- store lifecycle */

pg_store_t*
pg_store_open(const char* dsn, const pg_ops_t* ops)
{
    pg_store_t* ps = calloc(1, sizeof *ps);
    if (ps == NULL) {
        return NULL;
    }

    if (ops != NULL) { /* fake mode: borrow the caller's ops table */
        ps->ops = *ops;
        ps->ctx = ops->ctx;
        ps->owns_ctx = 0;
        return ps;
    }

    struct pq_ctx* px = calloc(1, sizeof *px);
    if (px == NULL) {
        free(ps);
        return NULL;
    }
    /* Bound connection setup so a PG network outage cannot stall every op
     * behind px->mtx for the OS-level socket timeout (P2). libpq's
     * connect_timeout defaults to 0 = wait forever; 5s is the worst case
     * we are willing to hold the store lock in one attempt. Operators who
     * already pinned a connect_timeout keep their value. */
    if (strstr(dsn, "connect_timeout") == NULL) {
        if (strncmp(dsn, "postgres://", 11) == 0 || strncmp(dsn, "postgresql://", 13) == 0) {
            /* URI form: connect_timeout must be a query parameter, not a
             * keyword token (libpq rejects mixed forms with "unexpected
             * spaces"). */
            const char* q = strchr(dsn, '?');
            size_t      base = q != NULL ? (size_t)(q - dsn) : strlen(dsn);
            const char* sep = q != NULL ? "&" : "?";
            snprintf(px->dsn, sizeof px->dsn, "%.*s%sconnect_timeout=5", (int)base, dsn, sep);
        } else {
            /* keyword/value form */
            snprintf(px->dsn, sizeof px->dsn, "%s connect_timeout=5", dsn);
        }
    } else {
        snprintf(px->dsn, sizeof px->dsn, "%s", dsn);
    }
    px->db = PQconnectdb(px->dsn);
    if (PQstatus(px->db) != CONNECTION_OK) {
        AIGATE_LOG_ERROR("pg_store_open: %s", PQerrorMessage(px->db));
        PQfinish(px->db);
        free(px);
        free(ps);
        return NULL;
    }
    pthread_mutex_init(&px->mtx, NULL);

    ps->ops.get_key_by_hash = pq_get_key_by_hash;
    ps->ops.list_keys = pq_list_keys;
    ps->ops.get_key_by_id = pq_get_key_by_id;
    ps->ops.list_models = pq_list_models;
    ps->ops.get_model = pq_get_model;
    ps->ops.create_key = pq_create_key;
    ps->ops.update_key = pq_update_key;
    ps->ops.revoke_key = pq_revoke_key;
    ps->ops.create_model = pq_create_model;
    ps->ops.update_model = pq_update_model;
    ps->ops.delete_model = pq_delete_model;
    ps->ops.list_providers = pq_list_providers;
    ps->ops.get_provider = pq_get_provider;
    ps->ops.create_provider = pq_create_provider;
    ps->ops.update_provider = pq_update_provider;
    ps->ops.delete_provider = pq_delete_provider;
    ps->ops.flush_usage = pq_flush_usage;
    ps->ops.query_usage = pq_query_usage;
    ps->ops.ctx = px;
    ps->ctx = px;
    ps->owns_ctx = 1;
    return ps;
}

void
pg_store_close(pg_store_t* ps)
{
    if (ps == NULL) {
        return;
    }
    if (ps->owns_ctx) {
        struct pq_ctx* px = ps->ctx;
        PQfinish(px->db);
        pthread_mutex_destroy(&px->mtx);
        free(px);
    }
    free(ps);
}

int
pg_store_migrate(pg_store_t* ps)
{
    if (ps == NULL) {
        return -1;
    }
    /* Fake stores cannot migrate; the caller manages their own schema. */
    if (ps->ops.flush_usage != pq_flush_usage) {
        return 0;
    }

    struct pq_ctx* px = ps->ctx;
    int            rc = 0;

    /* SCHEMA_SQL is idempotent (IF NOT EXISTS / ON CONFLICT DO NOTHING),
     * so always apply it: databases created before v2/v3 still need the
     * ALTERs even though version 1 is already recorded. */
    pq_lock(px);
    {
        PGresult* begin = PQexec(px->db, "BEGIN");
        PGresult* body = begin != NULL ? PQexec(px->db, SCHEMA_SQL) : NULL;
        /* On any failure the transaction is aborted; COMMIT would be a
         * no-op, so roll back and surface the error instead of starting
         * the gateway on a half-applied schema. */
        PGresult* end =
            (body != NULL && PQresultStatus(body) == PGRES_COMMAND_OK) ? PQexec(px->db, "COMMIT")
                                                                       : PQexec(px->db, "ROLLBACK");
        if (begin != NULL && PQresultStatus(begin) != PGRES_COMMAND_OK) {
            AIGATE_LOG_ERROR("pg migrate: BEGIN failed: %s", PQerrorMessage(px->db));
            rc = -1;
        } else if (body == NULL || PQresultStatus(body) != PGRES_COMMAND_OK) {
            AIGATE_LOG_ERROR("pg migrate: schema apply failed: %s", PQerrorMessage(px->db));
            rc = -1;
        } else if (end != NULL && PQresultStatus(end) != PGRES_COMMAND_OK) {
            AIGATE_LOG_ERROR("pg migrate: COMMIT/ROLLBACK failed: %s", PQerrorMessage(px->db));
            rc = -1;
        }
        PQclear(begin);
        PQclear(body);
        PQclear(end);
    }
    pq_unlock(px);
    return rc;
}

const pg_ops_t*
pg_store_ops(const pg_store_t* ps)
{
    return ps != NULL ? &ps->ops : NULL;
}
