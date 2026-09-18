/** @file aigate_core.c
 *  @brief Pipeline implementation (see aigate_core.h). */
#include "aigate_core.h"
#include "aigate_log.h"
#include "model_router.h"
#include "provider_openai.h"
#include "upstream_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int
aigate_core_init(aigate_core*   ac,
                 pg_store_t*    ps,
                 const uint8_t* master32,
                 int            default_timeout_ms,
                 int            flush_interval_s)
{
    memset(ac, 0, sizeof *ac);
    if (auth_key_init(&ac->keys, ps) != 0) {
        return -1;
    }
    ac->rl = ratelimit_new();
    ac->router = model_router_new(ps, master32);
    ac->um = usage_meter_new(ps, flush_interval_s);
    ac->default_timeout_ms = default_timeout_ms;
    if (ac->rl == NULL || ac->router == NULL || ac->um == NULL) {
        return -1;
    }
    return 0;
}

void
aigate_core_shutdown(aigate_core* ac)
{
    if (ac->um != NULL) {
        usage_meter_free(ac->um);
    }
    if (ac->router != NULL) {
        model_router_free(ac->router);
    }
    if (ac->rl != NULL) {
        ratelimit_free(ac->rl);
    }
    auth_key_shutdown(&ac->keys);
}

int
aigate_write_json(aigate_response_ctx* rc, int status, const char* body, size_t len)
{
    if (!rc->headers_sent) {
        rc->status = status;
        rc->set_header(rc->impl, "Content-Type", "application/json");
        char cl[32];
        snprintf(cl, sizeof cl, "%zu", len);
        rc->set_header(rc->impl, "Content-Length", cl);
        rc->headers_sent = true;
    }
    return rc->write(rc->impl, body, len, true);
}

int
aigate_write_error(aigate_response_ctx* rc, int http_status, const char* type, const char* message)
{
    json_t* err = json_object();
    json_object_set_new(err, "message", json_string(message));
    json_object_set_new(err, "type", json_string(type));
    json_object_set_new(err, "code", json_integer(http_status));
    json_t* root = json_object();
    json_object_set_new(root, "error", err);
    char* packed = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (packed == NULL) {
        return -1;
    }
    int rv = aigate_write_json(rc, http_status, packed, strlen(packed));
    free(packed);
    return rv;
}

/* pipeline exit codes */
enum {
    PIPE_OK = 0,
    PIPE_AUTH = 401,
    PIPE_FORBIDDEN = 403,
    PIPE_RATE = 429,
    PIPE_MODEL = 404,
    PIPE_UPSTREAM = 502,
    PIPE_UNSUPPORTED = 501,
};

int
aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    /* --- auth --- */
    key_rec_t krec;
    int       arc = auth_key_resolve(&ac->keys, rq->bearer, &krec);
    if (arc != 0) {
        aigate_write_error(rc, PIPE_AUTH, "auth_error", "invalid api key");
        key_rec_free(&krec);
        return 0;
    }

    /* --- model + allowlist (parsed from request body) --- */
    const char* model = "";
    json_t*     jbody = NULL;
    if (rq->body != NULL && rq->body_len > 0) {
        jbody = json_loads((const char*)rq->body, 0, NULL);
    }
    if (jbody != NULL) {
        json_t* jm = json_object_get(jbody, "model");
        if (jm != NULL && json_is_string(jm)) {
            model = json_string_value(jm);
        }
    }
    if (model[0] == '\0' || !key_allows_model(&krec, model)) {
        aigate_write_error(rc, PIPE_FORBIDDEN, "auth_error", "model not allowed for this key");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- rate limit --- */
    long retry_ms = 0;
    int  rrc = rl_allow_request(ac->rl, krec.key_id, krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) {
            ra_s = 1;
        }
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_error(rc, PIPE_RATE, "rate_limit", "rate limit exceeded");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- route --- */
    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_error(rc, PIPE_MODEL, "model_not_found", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- provider build --- */
    if (!provider_openai_supports(route.provider)) {
        aigate_write_error(
            rc, PIPE_UNSUPPORTED, "unsupported_provider", "provider not supported by this build");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }
    /* upstream path mirrors the gateway path (spec §4.1 non-streaming set) */
    const char* up_path = "/chat/completions";
    if (strcmp(rq->path, "/v1/embeddings") == 0) {
        up_path = "/embeddings";
    } else if (strcmp(rq->path, "/v1/completions") == 0) {
        up_path = "/completions";
    }

    char   url[1024];
    char*  merged = NULL;
    size_t mlen = 0;
    if (provider_openai_build(&route,
                              up_path,
                              rq->body != NULL ? (const char*)rq->body : NULL,
                              url,
                              sizeof url,
                              &merged,
                              &mlen) != 0) {
        aigate_write_error(rc, 500, "internal", "failed to build upstream request");
        json_decref(jbody);
        key_rec_free(&krec);
        free(merged);
        return 0;
    }

    /* --- upstream call with a single retry on 5xx --- */
    int      status = 0;
    char*    ubody = NULL;
    size_t   ulen = 0;
    uint64_t t0 = mono_ns();
    int      urc = upstream_call(
        url, route.upstream_key, merged, mlen, ac->default_timeout_ms, &status, &ubody, &ulen);
    if (urc == 0 && status >= 500) {
        free(ubody);
        ubody = NULL;
        struct timespec sl = {0, 200 * 1000000}; /* 200ms */
        nanosleep(&sl, NULL);
        urc = upstream_call(
            url, route.upstream_key, merged, mlen, ac->default_timeout_ms, &status, &ubody, &ulen);
    }
    uint64_t lat = mono_ns() - t0;
    free(merged);

    if (urc != 0 || status >= 500) {
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", route.provider);
        }
        aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
        /* still meter the failure */
        um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, 0, 0, lat, route.provider);
        free(ubody);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- parse usage (missing → 0/0) --- */
    long ptok = 0, ctok = 0;
    if (ubody != NULL) {
        json_t* jup = json_loads(ubody, 0, NULL);
        if (jup != NULL) {
            json_t* jusage = json_object_get(jup, "usage");
            if (jusage != NULL) {
                json_t* jp = json_object_get(jusage, "prompt_tokens");
                json_t* jc = json_object_get(jusage, "completion_tokens");
                if (json_is_integer(jp)) {
                    ptok = json_integer_value(jp);
                }
                if (json_is_integer(jc)) {
                    ctok = json_integer_value(jc);
                }
            }
            json_decref(jup);
        } else {
            AIGATE_LOG_WARN("upstream body for %s is not JSON", model);
        }
    }
    um_record(ac->um, krec.key_id, model, status, ptok, ctok, lat, route.provider);
    rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

    int rv = aigate_write_json(rc, status, ubody ? ubody : "", ulen);
    free(ubody);
    json_decref(jbody);
    key_rec_free(&krec);
    return rv;
}
