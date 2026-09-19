/** @file aigate_core.c
 *  @brief Pipeline implementation (see aigate_core.h). */
#include "aigate_core.h"
#include "aigate_log.h"
#include "model_router.h"
#include "provider_anthropic.h"
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

typedef struct {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    char                 line_buf[4096];
    size_t               line_len;
    long                 prompt_tokens;
    long                 completion_tokens;
    bool                 has_usage;
} stream_accum_t;

static void
stream_process_line(stream_accum_t* acc, const char* line)
{
    const char* d = strstr(line, "data:");
    if (d == NULL) {
        return;
    }
    const char* u = strstr(d, "\"usage\"");
    if (u == NULL) {
        return;
    }
    const char* jstart = strchr(d, '{');
    if (jstart == NULL) {
        return;
    }
    json_t* root = json_loads(jstart, 0, NULL);
    if (root == NULL) {
        return;
    }
    json_t* jusage = json_object_get(root, "usage");
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jp = json_object_get(jusage, "prompt_tokens");
        json_t* jc = json_object_get(jusage, "completion_tokens");
        if (json_is_integer(jp)) {
            acc->prompt_tokens = json_integer_value(jp);
            acc->has_usage = true;
        }
        if (json_is_integer(jc)) {
            acc->completion_tokens = json_integer_value(jc);
            acc->has_usage = true;
        }
    }
    json_decref(root);
}

static int
stream_chunk_handler(void* user_data, const void* chunk, size_t len)
{
    stream_accum_t* acc = user_data;
    if (!acc->headers_sent) {
        acc->rc->status = 200;
        if (acc->rc->set_header != NULL) {
            acc->rc->set_header(acc->rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
            acc->rc->set_header(acc->rc->impl, "Cache-Control", "no-cache");
            acc->rc->set_header(acc->rc->impl, "Connection", "keep-alive");
        }
        acc->headers_sent = true;
        acc->rc->headers_sent = true;
    }

    if (acc->rc->write != NULL && len > 0) {
        if (acc->rc->write(acc->rc->impl, chunk, len, false) != 0) {
            return -1;
        }
    }

    const char* p = chunk;
    const char* end = p + len;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (acc->line_len + seg < sizeof(acc->line_buf)) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';
                stream_process_line(acc, acc->line_buf);
            }
            acc->line_len = 0;
            p = nl + 1;
        } else {
            size_t seg = (size_t)(end - p);
            if (acc->line_len + seg < sizeof(acc->line_buf) - 1) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';
            } else {
                acc->line_len = 0;
            }
            p = end;
        }
    }
    return 0;
}

static int
anthropic_stream_chunk_handler(void* user_data, const void* chunk, size_t len)
{
    anthropic_bridge_t* b = user_data;
    return anthropic_bridge_feed(b, chunk, len);
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
    ac->ps = ps;
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

    /* --- handle GET /v1/models (data plane: list allowed enabled models) --- */
    if (rq->path != NULL && strcmp(rq->path, "/v1/models") == 0) {
        if (rq->method != NULL && strcmp(rq->method, "GET") == 0) {
            model_rec_t     recs[256];
            int             n = 0;
            const pg_ops_t* ops = ac->ps != NULL ? pg_store_ops(ac->ps) : NULL;
            if (ops != NULL && ops->list_models != NULL) {
                ops->list_models(ops->ctx, recs, 256, &n);
            }
            json_t* arr = json_array();
            for (int i = 0; i < n; i++) {
                if (recs[i].enabled && key_allows_model(&krec, recs[i].name)) {
                    json_t* obj = json_object();
                    json_object_set_new(obj, "id", json_string(recs[i].name));
                    json_object_set_new(obj, "object", json_string("model"));
                    json_object_set_new(obj, "created", json_integer(0));
                    json_object_set_new(
                        obj, "owned_by", json_string(recs[i].provider[0] ? recs[i].provider : "system"));
                    json_array_append_new(arr, obj);
                }
                model_rec_free(&recs[i]);
            }
            json_t* root = json_object();
            json_object_set_new(root, "object", json_string("list"));
            json_object_set_new(root, "data", arr);
            char* packed = json_dumps(root, JSON_COMPACT);
            json_decref(root);
            key_rec_free(&krec);
            if (packed == NULL) {
                return aigate_write_error(rc, 500, "internal_error", "json encode failed");
            }
            int rv = aigate_write_json(rc, 200, packed, strlen(packed));
            free(packed);
            return rv;
        }
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

    /* --- provider check & build --- */
    int is_openai = provider_openai_supports(route.provider);
    int is_anthropic = provider_anthropic_supports(route.provider);
    if (!is_openai && !is_anthropic) {
        aigate_write_error(
            rc, PIPE_UNSUPPORTED, "unsupported_provider", "provider not supported by this build");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    char        url[1024];
    char*       merged = NULL;
    size_t      mlen = 0;
    const char* extra_hdrs[4][2] = {{0}};
    int         n_extra_hdrs = 0;

    if (is_openai) {
        const char* up_path = "/chat/completions";
        if (strcmp(rq->path, "/v1/embeddings") == 0) {
            up_path = "/embeddings";
        } else if (strcmp(rq->path, "/v1/completions") == 0) {
            up_path = "/completions";
        }
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
    } else if (is_anthropic) {
        if (provider_anthropic_build(&route,
                                     rq->body != NULL ? (const char*)rq->body : NULL,
                                     url,
                                     sizeof url,
                                     extra_hdrs,
                                     &n_extra_hdrs,
                                     &merged,
                                     &mlen) != 0) {
            aigate_write_error(rc, 500, "internal", "failed to build upstream request");
            json_decref(jbody);
            key_rec_free(&krec);
            free(merged);
            return 0;
        }
    }

    /* Check if streaming */
    json_t* jstream = (jbody != NULL) ? json_object_get(jbody, "stream") : NULL;
    bool is_streaming = (jstream != NULL && json_is_true(jstream));

    if (is_streaming) {
        stream_accum_t     s_acc;
        memset(&s_acc, 0, sizeof s_acc);
        anthropic_bridge_t bridge;
        upstream_chunk_fn  chunk_fn = stream_chunk_handler;
        void*              chunk_ctx = &s_acc;
        s_acc.rc = rc;

        if (is_anthropic) {
            anthropic_bridge_init(&bridge, rc);
            chunk_fn = anthropic_stream_chunk_handler;
            chunk_ctx = &bridge;
        }

        int      status = 0;
        uint64_t t0 = mono_ns();
        long     silence_timeout_ms =
            ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
        int      urc = upstream_stream_call(
            url, route.upstream_key, extra_hdrs, n_extra_hdrs, merged, mlen, silence_timeout_ms,
            chunk_fn, chunk_ctx, &status);
        bool headers_sent = is_anthropic ? bridge.headers_sent : s_acc.headers_sent;

        if (!headers_sent && (urc != 0 || status >= 500)) {
            struct timespec sl = {0, 200 * 1000000}; /* 200ms */
            nanosleep(&sl, NULL);
            urc = upstream_stream_call(
                url, route.upstream_key, extra_hdrs, n_extra_hdrs, merged, mlen, silence_timeout_ms,
                chunk_fn, chunk_ctx, &status);
            headers_sent = is_anthropic ? bridge.headers_sent : s_acc.headers_sent;
        }
        uint64_t lat = mono_ns() - t0;
        free(merged);

        if (!headers_sent) {
            if (rc->set_header != NULL) {
                rc->set_header(rc->impl, "X-Upstream-Provider", route.provider);
            }
            aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
            um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, 0, 0, lat, route.provider);
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        long ptok = is_anthropic ? bridge.input_tokens : s_acc.prompt_tokens;
        long ctok = is_anthropic ? bridge.output_tokens : s_acc.completion_tokens;

        if (urc != 0) {
            const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                : "stream interrupted: transport error";
            char sse_err[256];
            snprintf(sse_err, sizeof sse_err,
                     "data: {\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\n"
                     "data: [DONE]\n\n",
                     err_msg);
            if (rc->write != NULL) {
                rc->write(rc->impl, sse_err, strlen(sse_err), true);
            }
            um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, ptok, ctok, lat, route.provider);
            if (ptok + ctok > 0) {
                rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            }
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        if (is_anthropic) {
            anthropic_bridge_finish(&bridge);
        } else if (rc->write != NULL) {
            rc->write(rc->impl, "", 0, true);
        }

        um_record(ac->um, krec.key_id, model, status > 0 ? status : 200, ptok, ctok, lat, route.provider);
        rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- upstream non-streaming call with a single retry on 5xx --- */
    int      status = 0;
    char*    ubody = NULL;
    size_t   ulen = 0;
    uint64_t t0 = mono_ns();
    int      urc = upstream_call_ext(
        url, route.upstream_key, extra_hdrs, n_extra_hdrs, merged, mlen, ac->default_timeout_ms, &status, &ubody, &ulen);
    if (urc == 0 && status >= 500) {
        free(ubody);
        ubody = NULL;
        struct timespec sl = {0, 200 * 1000000}; /* 200ms */
        nanosleep(&sl, NULL);
        urc = upstream_call_ext(
            url, route.upstream_key, extra_hdrs, n_extra_hdrs, merged, mlen, ac->default_timeout_ms, &status, &ubody, &ulen);
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

    if (is_anthropic) {
        char*  oai_resp = NULL;
        size_t oai_len = 0;
        long   ptok = 0, ctok = 0;
        if (provider_anthropic_resp_to_openai(ubody ? ubody : "", model, &oai_resp, &oai_len, &ptok, &ctok) != 0) {
            aigate_write_error(rc, 502, "upstream_error", "failed to parse anthropic response");
            free(ubody);
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }
        um_record(ac->um, krec.key_id, model, status, ptok, ctok, lat, route.provider);
        rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
        int rv = aigate_write_json(rc, status, oai_resp, oai_len);
        free(oai_resp);
        free(ubody);
        json_decref(jbody);
        key_rec_free(&krec);
        return rv;
    }

    /* --- OpenAI parse usage (missing → 0/0) --- */
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
