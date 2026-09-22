/** @file aigate_core.c
 *  @brief Pipeline implementation (see aigate_core.h). */
#include "aigate_core.h"
#include "aigate_log.h"
#include "metrics.h"
#include "model_router.h"
#include "provider_adapter.h"
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
    ac->um = usage_meter_new(ps, ac->rl, flush_interval_s);
    ac->cb = cb_create();
    ac->ps = ps;
    ac->default_timeout_ms = default_timeout_ms;
    if (ac->rl == NULL || ac->router == NULL || ac->um == NULL || ac->cb == NULL) {
        /* Roll back any partially built sub-objects; the router teardown
         * also cleanses its master-key copy. */
        if (ac->cb != NULL) {
            cb_destroy(ac->cb);
        }
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
        memset(ac, 0, sizeof *ac);
        return -1;
    }
    return 0;
}

void
aigate_core_shutdown(aigate_core* ac)
{
    if (ac->cb != NULL) {
        cb_destroy(ac->cb);
        ac->cb = NULL;
    }
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

    /* --- rate limit (before /v1/models and model routing so every data-plane
     *  request, including GET /v1/models, counts against the key's QPS) --- */
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
        key_rec_free(&krec);
        return 0;
    }
    /* --- daily token quota gate (after the QPS gate, before /v1/models so
     *  the limit applies uniformly to all data-plane traffic) --- */
    if (krec.daily_token_quota > 0 &&
        rl_remaining_daily(ac->rl, krec.key_id, krec.daily_token_quota) <= 0) {
        time_t now = time(NULL);
        time_t next = (time_t)(now - (now % 86400)) + 86400; /* next UTC midnight */
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", (long)(next - now));
        rc->set_header(rc->impl, "Retry-After", ra);
        aigate_write_error(rc, PIPE_RATE, "daily_quota_exceeded", "daily token quota exceeded");
        key_rec_free(&krec);
        return 0;
    }
    /* --- handle GET /v1/models (data plane: list allowed enabled models) --- */
    if (rq->path != NULL && strcmp(rq->path, "/v1/models") == 0) {
        if (rq->method != NULL && strcmp(rq->method, "GET") == 0) {
            model_rec_t* recs = calloc(256, sizeof(model_rec_t));
            if (recs == NULL) {
                key_rec_free(&krec);
                return aigate_write_error(rc, 500, "internal_error", "out of memory");
            }
            int             n = 0;
            const pg_ops_t* ops = ac->ps != NULL ? pg_store_ops(ac->ps) : NULL;
            if (ops != NULL && ops->list_models != NULL) {
                if (ops->list_models(ops->ctx, recs, 256, &n) != 0) {
                    /* Storage failure: distinguish "no models" from "PG is
                     * down" so callers do not mistake an outage for an empty
                     * catalog. */
                    free(recs);
                    key_rec_free(&krec);
                    return aigate_write_error(rc, 503, "internal_error",
                                              "model list unavailable");
                }
            }
            json_t* arr = json_array();
            for (int i = 0; i < n; i++) {
                if (recs[i].enabled && key_allows_model(&krec, recs[i].name)) {
                    json_t* obj = json_object();
                    json_object_set_new(obj, "id", json_string(recs[i].name));
                    json_object_set_new(obj, "object", json_string("model"));
                    json_object_set_new(obj, "created", json_integer(0));
                    json_object_set_new(
                        obj,
                        "owned_by",
                        json_string(recs[i].provider[0] ? recs[i].provider : "system"));
                    json_array_append_new(arr, obj);
                }
                model_rec_free(&recs[i]);
            }
            free(recs);
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

    /* --- route --- */
    model_rec_t route;
    if (model_router_resolve(ac->router, model, &route) != 0) {
        aigate_write_error(rc, PIPE_MODEL, "model_not_found", "model not found");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- candidate targets selection --- */
    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int               n_candidates = 0;
    if (model_router_select_candidates(
            ac->cb, &route, candidates, MAX_TARGETS_PER_MODEL, &n_candidates) != 0 ||
        n_candidates == 0) {
        aigate_write_error(
            rc, PIPE_MODEL, "no_healthy_upstream", "no upstream targets available for model");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- handle /v1/embeddings --- */
    if (rq->path != NULL && strcmp(rq->path, "/v1/embeddings") == 0) {
        int n_supported = 0;
        for (int ci = 0; ci < n_candidates; ci++) {
            const provider_adapter_t* adapter = provider_find(candidates[ci].provider);
            if (adapter != NULL && adapter->build_embeddings != NULL &&
                adapter->parse_embeddings_response != NULL) {
                n_supported++;
            }
        }
        if (n_supported == 0) {
            aigate_write_error(
                rc, 400, "unsupported_endpoint", "model or provider does not support embeddings");
            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        uint64_t    total_lat = 0;
        const char* last_provider = route.provider;

        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t*        target = &candidates[ci];
            const provider_adapter_t* adapter = provider_find(target->provider);
            if (adapter == NULL || adapter->build_embeddings == NULL ||
                adapter->parse_embeddings_response == NULL) {
                continue;
            }

            model_rec_t cur_route = route;
            snprintf(cur_route.provider,
                     sizeof cur_route.provider,
                     "%.*s",
                     (int)sizeof cur_route.provider - 1,
                     target->provider);
            snprintf(cur_route.endpoint,
                     sizeof cur_route.endpoint,
                     "%.*s",
                     (int)sizeof cur_route.endpoint - 1,
                     target->endpoint);
            snprintf(cur_route.upstream_key,
                     sizeof cur_route.upstream_key,
                     "%.*s",
                     (int)sizeof cur_route.upstream_key - 1,
                     target->upstream_key);
            last_provider = target->provider;

            char        url[1024];
            char*       merged = NULL;
            size_t      mlen = 0;
            const char* extra_hdrs[4][2] = {{0}};
            int         n_extra_hdrs = 0;

            if (adapter->build_embeddings(&cur_route,
                                          rq->body != NULL ? (const char*)rq->body : NULL,
                                          url,
                                          sizeof url,
                                          extra_hdrs,
                                          &n_extra_hdrs,
                                          &merged,
                                          &mlen) != 0) {
                free(merged);
                continue;
            }

            int      status = 0;
            char*    ubody = NULL;
            size_t   ulen = 0;
            uint64_t t0 = mono_ns();
            int      urc = upstream_call_ext(url,
                                             cur_route.upstream_key,
                                             extra_hdrs,
                                             n_extra_hdrs,
                                             merged,
                                             mlen,
                                             ac->default_timeout_ms,
                                             &status,
                                             &ubody,
                                             &ulen);

            if (n_candidates == 1 && urc == 0 && status >= 500) {
                free(ubody);
                ubody = NULL;
                struct timespec sl = {0, 200 * 1000000}; /* 200ms */
                nanosleep(&sl, NULL);
                urc = upstream_call_ext(url,
                                        cur_route.upstream_key,
                                        extra_hdrs,
                                        n_extra_hdrs,
                                        merged,
                                        mlen,
                                        ac->default_timeout_ms,
                                        &status,
                                        &ubody,
                                        &ulen);
            }
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;
            free(merged);

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
            if (!is_failover && status < 400) {
                cb_record_success(ac->cb, model, target->endpoint);
                char*  parsed_body = NULL;
                size_t parsed_len = 0;
                long   ptok = 0;
                int    parsed_status = status;
                if (adapter->parse_embeddings_response(ubody ? ubody : "",
                                                       ulen,
                                                       model,
                                                       &parsed_status,
                                                       &parsed_body,
                                                       &parsed_len,
                                                       &ptok) != 0) {
                    free(ubody);
                    aigate_write_error(
                        rc, 502, "upstream_error", "failed to parse upstream embeddings response");
                    json_decref(jbody);
                    key_rec_free(&krec);
                    return 0;
                }
                free(ubody);

                um_record(ac->um,
                          krec.key_id,
                          model,
                          parsed_status,
                          ptok,
                          0,
                          0,
                          total_lat,
                          target->provider);
                if (ptok > 0) {
                    rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok);
                }

                int rv = aigate_write_json(
                    rc, parsed_status, parsed_body ? parsed_body : "", parsed_len);
                free(parsed_body);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            }

            cb_record_failure(ac->cb, model, target->endpoint, status);

            if (!is_failover && status >= 400) {
                if (ubody != NULL && urc == 0) {
                    um_record(
                        ac->um, krec.key_id, model, status, 0, 0, 0, total_lat, target->provider);
                    int rv = aigate_write_json(rc, status, ubody, ulen);
                    free(ubody);
                    json_decref(jbody);
                    key_rec_free(&krec);
                    return rv;
                }
                free(ubody);
                break;
            }
            free(ubody);

            if (ci + 1 < n_candidates) {
                AIGATE_LOG_WARN("failover embeddings for model %s from %s (%s) to %s (%s) due to "
                                "status %d (urc %d)",
                                model,
                                target->provider,
                                target->endpoint,
                                candidates[ci + 1].provider,
                                candidates[ci + 1].endpoint,
                                status,
                                urc);
                metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                continue;
            }
        }

        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        }
        aigate_write_error(
            rc, PIPE_UPSTREAM, "upstream_error", "upstream embeddings request failed");
        um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, total_lat, last_provider);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    int n_chat_supported = 0;
    for (int ci = 0; ci < n_candidates; ci++) {
        const provider_adapter_t* adapter = provider_find(candidates[ci].provider);
        if (adapter != NULL && adapter->build_chat != NULL) {
            n_chat_supported++;
        }
    }
    if (n_chat_supported == 0) {
        aigate_write_error(
            rc, PIPE_UNSUPPORTED, "unsupported_provider", "provider not supported by this build");
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* Check if streaming */
    json_t* jstream = (jbody != NULL) ? json_object_get(jbody, "stream") : NULL;
    bool    is_streaming = (jstream != NULL && json_is_true(jstream));

    if (is_streaming) {
        uint64_t    total_lat = 0;
        const char* last_provider = route.provider;

        for (int ci = 0; ci < n_candidates; ci++) {
            upstream_target_t*        target = &candidates[ci];
            const provider_adapter_t* adapter = provider_find(target->provider);
            if (adapter == NULL || adapter->build_chat == NULL ||
                adapter->stream_bridge_new == NULL) {
                continue;
            }

            model_rec_t cur_route = route;
            snprintf(cur_route.provider,
                     sizeof cur_route.provider,
                     "%.*s",
                     (int)sizeof cur_route.provider - 1,
                     target->provider);
            snprintf(cur_route.endpoint,
                     sizeof cur_route.endpoint,
                     "%.*s",
                     (int)sizeof cur_route.endpoint - 1,
                     target->endpoint);
            snprintf(cur_route.upstream_key,
                     sizeof cur_route.upstream_key,
                     "%.*s",
                     (int)sizeof cur_route.upstream_key - 1,
                     target->upstream_key);
            last_provider = target->provider;

            char        url[1024];
            char*       merged = NULL;
            size_t      mlen = 0;
            const char* extra_hdrs[4][2] = {{0}};
            int         n_extra_hdrs = 0;

            if (adapter->build_chat(&cur_route,
                                    rq->body != NULL ? (const char*)rq->body : NULL,
                                    url,
                                    sizeof url,
                                    extra_hdrs,
                                    &n_extra_hdrs,
                                    &merged,
                                    &mlen) != 0) {
                free(merged);
                continue;
            }

            stream_bridge_t* bridge = adapter->stream_bridge_new(rc, model);
            if (bridge == NULL) {
                free(merged);
                continue;
            }

            int      status = 0;
            char*    sbody = NULL;
            size_t   slen = 0;
            uint64_t t0 = mono_ns();
            long     silence_timeout_ms =
                ac->default_timeout_ms > 0 ? (long)ac->default_timeout_ms : 30000L;
            int  urc = upstream_stream_call(url,
                                            cur_route.upstream_key,
                                            extra_hdrs,
                                            n_extra_hdrs,
                                            merged,
                                            mlen,
                                            silence_timeout_ms,
                                            (upstream_chunk_fn)adapter->stream_bridge_feed,
                                            bridge,
                                            &status,
                                            &sbody,
                                            &slen);
            bool headers_sent = adapter->stream_bridge_headers_sent(bridge);

            if (n_candidates == 1 && !headers_sent && (urc != 0 || status >= 500)) {
                struct timespec sl = {0, 200 * 1000000}; /* 200ms */
                nanosleep(&sl, NULL);
                free(sbody); /* the retry re-captures into the same pointers */
                sbody = NULL;
                urc = upstream_stream_call(url,
                                           cur_route.upstream_key,
                                           extra_hdrs,
                                           n_extra_hdrs,
                                           merged,
                                           mlen,
                                           silence_timeout_ms,
                                           (upstream_chunk_fn)adapter->stream_bridge_feed,
                                           bridge,
                                           &status,
                                           &sbody,
                                           &slen);
                headers_sent = adapter->stream_bridge_headers_sent(bridge);
            }
            uint64_t lat = mono_ns() - t0;
            total_lat += lat;
            free(merged);

            bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

            if (!headers_sent) {
                cb_record_failure(ac->cb, model, target->endpoint, status);
                adapter->stream_bridge_free(bridge);

                if (!is_failover && status >= 400) {
                    /* Pre-headers 4xx: surface the upstream's own error body
                     * (mirrors the non-streaming passthrough). Only when a
                     * body was actually received (urc == 0). */
                    if (sbody != NULL && urc == 0) {
                        um_record(
                            ac->um, krec.key_id, model, status, 0, 0, 0, total_lat, target->provider);
                        int rv = aigate_write_json(rc, status, sbody, slen);
                        free(sbody);
                        json_decref(jbody);
                        key_rec_free(&krec);
                        return rv;
                    }
                    free(sbody);
                    break; /* no error body: fall through to the generic 502 */
                }

                if (ci + 1 < n_candidates) {
                    AIGATE_LOG_WARN("streaming failover for model %s from %s (%s) to %s (%s) due "
                                    "to status %d (urc %d)",
                                    model,
                                    target->provider,
                                    target->endpoint,
                                    candidates[ci + 1].provider,
                                    candidates[ci + 1].endpoint,
                                    status,
                                    urc);
                    metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
                    free(sbody);
                    continue;
                }
                free(sbody);
                break;
            }

            long ptok = 0, ctok = 0, cached_tok = 0;
            adapter->stream_bridge_get_tokens(bridge, &ptok, &ctok, &cached_tok);

            if (urc != 0) {
                const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                    : "stream interrupted: transport error";
                char        sse_err[256];
                snprintf(
                    sse_err,
                    sizeof sse_err,
                    "data: "
                    "{\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\n"
                    "data: [DONE]\n\n",
                    err_msg);
                if (rc->write != NULL) {
                    rc->write(rc->impl, sse_err, strlen(sse_err), true);
                }
                um_record(ac->um,
                          krec.key_id,
                          model,
                          PIPE_UPSTREAM,
                          ptok,
                          ctok,
                          cached_tok,
                          total_lat,
                          target->provider);
                if (ptok + ctok > 0) {
                    rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
                }
                adapter->stream_bridge_free(bridge);
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }

            cb_record_success(ac->cb, model, target->endpoint);
            adapter->stream_bridge_finish(bridge);
            um_record(ac->um,
                      krec.key_id,
                      model,
                      status > 0 ? status : 200,
                      ptok,
                      ctok,
                      cached_tok,
                      total_lat,
                      target->provider);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);
            adapter->stream_bridge_free(bridge);

            json_decref(jbody);
            key_rec_free(&krec);
            return 0;
        }

        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
        }
        aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
        um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, total_lat, last_provider);
        json_decref(jbody);
        key_rec_free(&krec);
        return 0;
    }

    /* --- upstream non-streaming call with failover loop --- */
    uint64_t    total_lat = 0;
    const char* last_provider = route.provider;

    for (int ci = 0; ci < n_candidates; ci++) {
        upstream_target_t*        target = &candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_chat == NULL ||
            adapter->parse_chat_response == NULL) {
            continue;
        }

        model_rec_t cur_route = route;
        snprintf(cur_route.provider,
                 sizeof cur_route.provider,
                 "%.*s",
                 (int)sizeof cur_route.provider - 1,
                 target->provider);
        snprintf(cur_route.endpoint,
                 sizeof cur_route.endpoint,
                 "%.*s",
                 (int)sizeof cur_route.endpoint - 1,
                 target->endpoint);
        snprintf(cur_route.upstream_key,
                 sizeof cur_route.upstream_key,
                 "%.*s",
                 (int)sizeof cur_route.upstream_key - 1,
                 target->upstream_key);
        last_provider = target->provider;

        char        url[1024];
        char*       merged = NULL;
        size_t      mlen = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        if (adapter->build_chat(&cur_route,
                                rq->body != NULL ? (const char*)rq->body : NULL,
                                url,
                                sizeof url,
                                extra_hdrs,
                                &n_extra_hdrs,
                                &merged,
                                &mlen) != 0) {
            free(merged);
            continue;
        }

        int      status = 0;
        char*    ubody = NULL;
        size_t   ulen = 0;
        uint64_t t0 = mono_ns();
        int      urc = upstream_call_ext(url,
                                         cur_route.upstream_key,
                                         extra_hdrs,
                                         n_extra_hdrs,
                                         merged,
                                         mlen,
                                         ac->default_timeout_ms,
                                         &status,
                                         &ubody,
                                         &ulen);

        if (n_candidates == 1 && urc == 0 && status >= 500) {
            free(ubody);
            ubody = NULL;
            struct timespec sl = {0, 200 * 1000000}; /* 200ms */
            nanosleep(&sl, NULL);
            urc = upstream_call_ext(url,
                                    cur_route.upstream_key,
                                    extra_hdrs,
                                    n_extra_hdrs,
                                    merged,
                                    mlen,
                                    ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(merged);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(ac->cb, model, target->endpoint);
            char*  parsed_body = NULL;
            size_t parsed_len = 0;
            long   ptok = 0, ctok = 0, cached_tok = 0;
            int    parsed_status = status;
            if (adapter->parse_chat_response(ubody ? ubody : "",
                                             ulen,
                                             model,
                                             &parsed_status,
                                             &parsed_body,
                                             &parsed_len,
                                             &ptok,
                                             &ctok,
                                             &cached_tok) != 0) {
                free(ubody);
                aigate_write_error(rc, 502, "upstream_error", "failed to parse upstream response");
                json_decref(jbody);
                key_rec_free(&krec);
                return 0;
            }
            free(ubody);

            um_record(ac->um,
                      krec.key_id,
                      model,
                      parsed_status,
                      ptok,
                      ctok,
                      cached_tok,
                      total_lat,
                      target->provider);
            rl_reserve_tokens(ac->rl, krec.key_id, krec.daily_token_quota, ptok + ctok);

            int rv =
                aigate_write_json(rc, parsed_status, parsed_body ? parsed_body : "", parsed_len);
            free(parsed_body);
            json_decref(jbody);
            key_rec_free(&krec);
            return rv;
        }

        cb_record_failure(ac->cb, model, target->endpoint, status);

        if (!is_failover && status >= 400) {
            /* Non-failover 4xx: surface the upstream's own error body to the
             * client (e.g. OpenAI "invalid request") instead of a generic 502.
             * Only when we actually received a body (urc == 0). */
            if (ubody != NULL && urc == 0) {
                um_record(ac->um, krec.key_id, model, status, 0, 0, 0, total_lat, target->provider);
                int rv = aigate_write_json(rc, status, ubody, ulen);
                free(ubody);
                json_decref(jbody);
                key_rec_free(&krec);
                return rv;
            }
            free(ubody);
            break; /* urc != 0 or no body: fall through to the generic 502 */
        }
        free(ubody);

        if (ci + 1 < n_candidates) {
            AIGATE_LOG_WARN(
                "failover for model %s from %s (%s) to %s (%s) due to status %d (urc %d)",
                model,
                target->provider,
                target->endpoint,
                candidates[ci + 1].provider,
                candidates[ci + 1].endpoint,
                status,
                urc);
            metrics_inc_failover(model, target->provider, candidates[ci + 1].provider);
            continue;
        }
    }

    if (rc->set_header != NULL) {
        rc->set_header(rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    um_record(ac->um, krec.key_id, model, PIPE_UPSTREAM, 0, 0, 0, total_lat, last_provider);
    json_decref(jbody);
    key_rec_free(&krec);
    return 0;
}
