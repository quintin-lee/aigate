/** @file pipeline_chat.c
 *  @brief /v1/chat/completions and /v1/models pipeline handling.
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "pg_store.h"
#include "provider_adapter.h"
#include "response_cache.h"
#include "upstream_client.h"
#include "upstream_hedged.h"
#include "latency_tracker.h"
#include "filter_chain.h"

#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief GET /v1/models handler. @return 1 when the path was handled, 0 to continue. */
int
handle_models_list(chat_req_t* q)
{
    aigate_request_ctx*  rq = q->rq;
    aigate_response_ctx* rc = q->rc;

    /* --- handle GET /v1/models (data plane: list allowed enabled models) --- */
    if (rq->path != NULL && strcmp(rq->path, "/v1/models") == 0) {
        if (rq->method != NULL && strcmp(rq->method, "GET") == 0) {
            model_rec_t* recs = calloc(256, sizeof(model_rec_t));
            if (recs == NULL) {
                aigate_write_error(rc, 500, "internal_error", "out of memory");
                return -1;
            }
            int             n = 0;
            const pg_ops_t* ops = q->ac->ps != NULL ? pg_store_ops(q->ac->ps) : NULL;
            if (ops != NULL && ops->list_models != NULL) {
                if (ops->list_models(ops->ctx, recs, 256, &n) != 0) {
                    /* Storage failure: distinguish "no models" from "PG is
                     * down" so callers do not mistake an outage for an empty
                     * catalog. */
                    free(recs);
                    aigate_write_error(rc, 503, "internal_error", "model list unavailable");
                    return -1;
                }
            }
            json_t* arr = json_array();
            for (int i = 0; i < n; i++) {
                if (recs[i].enabled && key_allows_model(&q->krec, recs[i].name)) {
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
            if (packed == NULL) {
                aigate_write_error(rc, 500, "internal_error", "json encode failed");
                return -1;
            }
            int rv = aigate_write_json(rc, 200, packed, strlen(packed));
            free(packed);
            (void)rv;
            return 1;
        }
    }
    return 0;
}

/** @brief Chat capability check + cache-control parse + cache lookup.
 *  @param[in,out] q            Request processing context.
 *  @param[out]    is_streaming set from body "stream" flag.
 *  @return 1 when the response was already written (HIT or unsupported);
 *          0 to continue to upstream; <0 never (reserved). */
int
prepare_chat_cache(chat_req_t* q, bool* is_streaming)
{
    int n_chat_supported = 0;
    for (int ci = 0; ci < q->n_candidates; ci++) {
        const provider_adapter_t* adapter = provider_find(q->candidates[ci].provider);
        if (adapter != NULL && adapter->build_chat != NULL) {
            n_chat_supported++;
        }
    }
    if (n_chat_supported == 0) {
        aigate_write_error(q->rc,
                           PIPE_UNSUPPORTED,
                           "unsupported_provider",
                           "provider not supported by this build");
        return 1;
    }

    /* Check if streaming */
    json_t* jstream = (q->jbody != NULL) ? json_object_get(q->jbody, "stream") : NULL;
    *is_streaming = (jstream != NULL && json_is_true(jstream));

    if (q->rq->cache_control != NULL) {
        if (strstr(q->rq->cache_control, "no-cache") != NULL ||
            strstr(q->rq->cache_control, "max-age=0") != NULL ||
            strcmp(q->rq->cache_control, "true") == 0 || strcmp(q->rq->cache_control, "1") == 0) {
            q->bypass_cache = true;
        }
        if (strstr(q->rq->cache_control, "no-store") != NULL) {
            q->bypass_cache = true;
            q->no_store = true;
        }
    }

    if (q->ac->rc != NULL && !q->bypass_cache) {
        response_cache_fingerprint(q->model, (const char*)q->eff_body, q->eff_len, q->cache_key);
    }
    if (q->rc->set_header != NULL && q->ac->rc != NULL && q->bypass_cache) {
        q->rc->set_header(q->rc->impl, "X-Cache", "MISS");
    }
    if (q->ac->rc != NULL && q->cache_key[0] != '\0') {
        cache_entry_t* ce = response_cache_get(q->ac->rc, q->cache_key);
        if (ce != NULL) {
            if (!*is_streaming) {
                if (q->rc->set_header != NULL) {
                    q->rc->set_header(q->rc->impl, "X-Cache", "HIT");
                    q->rc->set_header(q->rc->impl, "X-Cache-Lookup-Time", "0.10ms");
                    char age_str[32];
                    snprintf(age_str, sizeof(age_str), "%ld", (long)(time(NULL) - ce->created_at));
                    q->rc->set_header(q->rc->impl, "Age", age_str);
                }
                record_usage_and_event(q->ac,
                                       q->krec.key_id,
                                       q->model,
                                       200,
                                       ce->prompt_tokens,
                                       ce->completion_tokens,
                                       0,
                                       0,
                                       100000ULL,
                                       "cache",
                                       q->guardrail_act,
                                       ce->cost_usd);
                if (q->ac->be != NULL) {
                    budget_enforce_record(q->ac->be,
                                          q->krec.key_id,
                                          q->krec.group_id,
                                          ce->cost_usd,
                                          ce->prompt_tokens + ce->completion_tokens);
                }
                rl_reserve_tokens(q->ac->rl,
                                  q->krec.key_id,
                                  q->krec.daily_token_quota,
                                  ce->prompt_tokens + ce->completion_tokens);
                aigate_write_json(q->rc, 200, ce->response_body, ce->response_len);
                response_cache_release_entry(ce);
                return 1;
            } else {
                cache_stream_replay(q->ac, q->rc, ce, q->model, &q->krec, q->guardrail_act);
                return 1;
            }
        } else {
            if (q->rc->set_header != NULL) {
                q->rc->set_header(q->rc->impl, "X-Cache", "MISS");
            }
        }
    }
    return 0;
}

/** @brief /v1/chat/completions non-streaming failover loop. @return transport rc. */
int
handle_chat_sync(chat_req_t* q)
{
    /* --- upstream non-streaming call with failover loop --- */
    uint64_t    total_lat = 0;
    const char* last_provider = q->route.provider;
    int         start_ci = 0;

    /* --- hedged speculative request if enabled and >= 2 candidates --- */
    if (q->route.hedged_enabled && q->n_candidates >= 2) {
        upstream_target_t*        target0 = &q->candidates[0];
        upstream_target_t*        target1 = &q->candidates[1];
        const provider_adapter_t* adapter0 = provider_find(target0->provider);
        const provider_adapter_t* adapter1 = provider_find(target1->provider);

        if (adapter0 != NULL && adapter0->build_chat != NULL && adapter1 != NULL &&
            adapter1->build_chat != NULL) {

            model_rec_t cur_route0 = q->route;
            fill_cur_route(&q->route, target0, &cur_route0);
            char        url0[1024];
            char*       merged0 = NULL;
            size_t      mlen0 = 0;
            const char* extra_hdrs0[4][2] = {{0}};
            int         n_extra_hdrs0 = 0;

            model_rec_t cur_route1 = q->route;
            fill_cur_route(&q->route, target1, &cur_route1);
            char        url1[1024];
            char*       merged1 = NULL;
            size_t      mlen1 = 0;
            const char* extra_hdrs1[4][2] = {{0}};
            int         n_extra_hdrs1 = 0;

            int brc0 = adapter0->build_chat(&cur_route0,
                                            q->eff_body != NULL ? (const char*)q->eff_body : NULL,
                                            url0,
                                            sizeof url0,
                                            extra_hdrs0,
                                            &n_extra_hdrs0,
                                            &merged0,
                                            &mlen0);
            int brc1 = adapter1->build_chat(&cur_route1,
                                            q->eff_body != NULL ? (const char*)q->eff_body : NULL,
                                            url1,
                                            sizeof url1,
                                            extra_hdrs1,
                                            &n_extra_hdrs1,
                                            &merged1,
                                            &mlen1);

            if (brc0 == 0 && brc1 == 0) {
                hedged_call_params_t hparams;
                memset(&hparams, 0, sizeof(hparams));
                hparams.model = q->model;
                hparams.lt = q->ac->lt;
                hparams.delay_ms = q->route.hedged_delay_ms;
                hparams.budget_pct = q->route.hedge_budget_pct;
                hparams.timeout_ms = q->ac->default_timeout_ms;
                hparams.has_secondary = true;

                snprintf(hparams.primary.url, sizeof(hparams.primary.url), "%s", url0);
                snprintf(hparams.primary.key,
                         sizeof(hparams.primary.key),
                         "%s",
                         cur_route0.upstream_key);
                snprintf(hparams.primary.endpoint,
                         sizeof(hparams.primary.endpoint),
                         "%s",
                         target0->endpoint);
                hparams.primary.payload = merged0;
                hparams.primary.payload_len = mlen0;
                for (int i = 0; i < n_extra_hdrs0 && i < HEDGED_MAX_EXTRA_HEADERS; i++) {
                    hparams.primary.extra_headers[i][0] = extra_hdrs0[i][0];
                    hparams.primary.extra_headers[i][1] = extra_hdrs0[i][1];
                }
                hparams.primary.n_extra_headers = n_extra_hdrs0;

                snprintf(hparams.secondary.url, sizeof(hparams.secondary.url), "%s", url1);
                snprintf(hparams.secondary.key,
                         sizeof(hparams.secondary.key),
                         "%s",
                         cur_route1.upstream_key);
                snprintf(hparams.secondary.endpoint,
                         sizeof(hparams.secondary.endpoint),
                         "%s",
                         target1->endpoint);
                hparams.secondary.payload = merged1;
                hparams.secondary.payload_len = mlen1;
                for (int i = 0; i < n_extra_hdrs1 && i < HEDGED_MAX_EXTRA_HEADERS; i++) {
                    hparams.secondary.extra_headers[i][0] = extra_hdrs1[i][0];
                    hparams.secondary.extra_headers[i][1] = extra_hdrs1[i][1];
                }
                hparams.secondary.n_extra_headers = n_extra_hdrs1;

                hedged_call_result_t hres;
                int                  hrc = upstream_call_hedged(&hparams, &hres);
                free(merged0);
                free(merged1);

                if (hres.was_hedged) {
                    metrics_inc_hedged_requests();
                    if (hres.winning_target_idx == 1 && hres.status == 200) {
                        metrics_inc_hedged_won();
                    }
                }

                upstream_target_t* win_target = (hres.winning_target_idx == 1) ? target1 : target0;
                const provider_adapter_t* win_adapter =
                    (hres.winning_target_idx == 1) ? adapter1 : adapter0;
                total_lat = hres.latency_ns;
                last_provider = win_target->provider;

                if (q->ac->lt != NULL && total_lat > 0) {
                    latency_tracker_record(q->ac->lt, q->model, win_target->endpoint, total_lat);
                }

                int    status = hres.status;
                char*  ubody = hres.body;
                size_t ulen = hres.body_len;
                int    urc = (hrc == 0 && ubody != NULL) ? 0 : -502;
                bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

                if (!is_failover && status < 400) {
                    cb_record_success(q->ac->cb, q->model, win_target->endpoint);
                    char*  parsed_body = NULL;
                    size_t parsed_len = 0;
                    long   ptok = 0, ctok = 0, cached_tok = 0;
                    int    parsed_status = status;
                    if (win_adapter->parse_chat_response(ubody ? ubody : "",
                                                         ulen,
                                                         q->model,
                                                         &parsed_status,
                                                         &parsed_body,
                                                         &parsed_len,
                                                         &ptok,
                                                         &ctok,
                                                         &cached_tok) != 0) {
                        free(ubody);
                        aigate_write_error(
                            q->rc, 502, "upstream_error", "failed to parse upstream response");
                        chat_req_cleanup(q);
                        return 0;
                    }
                    free(ubody);

                    if (parsed_status == 200) {
                        char*  filtered_resp = NULL;
                        size_t filtered_len = 0;
                        if (filter_chain_execute_outbound(
                                q, parsed_body, parsed_len, &filtered_resp, &filtered_len) ==
                            FILTER_STOP) {
                            free(parsed_body);
                            chat_req_cleanup(q);
                            return 0;
                        }
                        if (filtered_resp != NULL) {
                            free(parsed_body);
                            parsed_body = filtered_resp;
                            parsed_len = filtered_len;
                        }
                    }

                    double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
                    record_usage_and_event(q->ac,
                                           q->krec.key_id,
                                           q->model,
                                           parsed_status,
                                           ptok,
                                           ctok,
                                           cached_tok,
                                           0,
                                           total_lat,
                                           win_target->provider,
                                           q->guardrail_act,
                                           req_cost);
                    if (q->ac->be != NULL) {
                        budget_enforce_record(
                            q->ac->be, q->krec.key_id, q->krec.group_id, req_cost, ptok + ctok);
                    }
                    rl_reserve_tokens(
                        q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);

                    if (q->ac->rc != NULL && q->cache_key[0] != '\0' && parsed_status == 200 &&
                        parsed_body != NULL && !q->no_store && parsed_len <= 1048576) {
                        response_cache_set(q->ac->rc,
                                           q->cache_key,
                                           q->model,
                                           parsed_body,
                                           parsed_len,
                                           ptok,
                                           ctok,
                                           req_cost,
                                           0);
                    }

                    int rv = aigate_write_json(
                        q->rc, parsed_status, parsed_body ? parsed_body : "", parsed_len);
                    free(parsed_body);
                    chat_req_cleanup(q);
                    return rv;
                }

                cb_record_failure(q->ac->cb, q->model, win_target->endpoint, status);
                if (!is_failover && status >= 400) {
                    if (ubody != NULL && urc == 0) {
                        record_usage_and_event(q->ac,
                                               q->krec.key_id,
                                               q->model,
                                               status,
                                               0,
                                               0,
                                               0,
                                               0,
                                               total_lat,
                                               win_target->provider,
                                               q->guardrail_act,
                                               0.0);
                        int rv = aigate_write_json(q->rc, status, ubody, ulen);
                        free(ubody);
                        chat_req_cleanup(q);
                        return rv;
                    }
                    free(ubody);
                } else {
                    free(ubody);
                }

                /* Both candidates 0 and 1 failed in the hedged run, fallback to candidate 2 if any */
                start_ci = 2;
            } else {
                free(merged0);
                free(merged1);
            }
        }
    }

    for (int ci = start_ci; ci < q->n_candidates; ci++) {
        upstream_target_t*        target = &q->candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_chat == NULL ||
            adapter->parse_chat_response == NULL) {
            continue;
        }

        model_rec_t cur_route = q->route;
        fill_cur_route(&q->route, target, &cur_route);
        last_provider = target->provider;

        char        url[1024];
        char*       merged = NULL;
        size_t      mlen = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        if (adapter->build_chat(&cur_route,
                                q->eff_body != NULL ? (const char*)q->eff_body : NULL,
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
                                         q->ac->default_timeout_ms,
                                         &status,
                                         &ubody,
                                         &ulen);

        if (q->n_candidates == 1 && urc == 0 && status >= 500) {
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
                                    q->ac->default_timeout_ms,
                                    &status,
                                    &ubody,
                                    &ulen);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        if (q->ac->lt != NULL && lat > 0) {
            latency_tracker_record(q->ac->lt, q->model, target->endpoint, lat);
        }
        free(merged);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(q->ac->cb, q->model, target->endpoint);
            char*  parsed_body = NULL;
            size_t parsed_len = 0;
            long   ptok = 0, ctok = 0, cached_tok = 0;
            int    parsed_status = status;
            if (adapter->parse_chat_response(ubody ? ubody : "",
                                             ulen,
                                             q->model,
                                             &parsed_status,
                                             &parsed_body,
                                             &parsed_len,
                                             &ptok,
                                             &ctok,
                                             &cached_tok) != 0) {
                free(ubody);
                aigate_write_error(
                    q->rc, 502, "upstream_error", "failed to parse upstream response");
                chat_req_cleanup(q);
                return 0;
            }
            free(ubody);

            if (parsed_status == 200) {
                char*  filtered_resp = NULL;
                size_t filtered_len = 0;
                if (filter_chain_execute_outbound(
                        q, parsed_body, parsed_len, &filtered_resp, &filtered_len) == FILTER_STOP) {
                    free(parsed_body);
                    chat_req_cleanup(q);
                    return 0;
                }
                if (filtered_resp != NULL) {
                    free(parsed_body);
                    parsed_body = filtered_resp;
                    parsed_len = filtered_len;
                }
            }

            double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
            record_usage_and_event(q->ac,
                                   q->krec.key_id,
                                   q->model,
                                   parsed_status,
                                   ptok,
                                   ctok,
                                   cached_tok,
                                   0,
                                   total_lat,
                                   target->provider,
                                   q->guardrail_act,
                                   req_cost);
            if (q->ac->be != NULL) {
                budget_enforce_record(
                    q->ac->be, q->krec.key_id, q->krec.group_id, req_cost, ptok + ctok);
            }
            rl_reserve_tokens(q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);

            if (q->ac->rc != NULL && q->cache_key[0] != '\0' && parsed_status == 200 &&
                parsed_body != NULL && !q->no_store && parsed_len <= 1048576) {
                response_cache_set(q->ac->rc,
                                   q->cache_key,
                                   q->model,
                                   parsed_body,
                                   parsed_len,
                                   ptok,
                                   ctok,
                                   req_cost,
                                   0);
            }

            int rv =
                aigate_write_json(q->rc, parsed_status, parsed_body ? parsed_body : "", parsed_len);
            free(parsed_body);
            chat_req_cleanup(q);
            return rv;
        }

        cb_record_failure(q->ac->cb, q->model, target->endpoint, status);

        if (!is_failover && status >= 400) {
            /* Non-failover 4xx: surface the upstream's own error body to the
             * client (e.g. OpenAI "invalid request") instead of a generic 502.
             * Only when we actually received a body (urc == 0). */
            if (ubody != NULL && urc == 0) {
                record_usage_and_event(q->ac,
                                       q->krec.key_id,
                                       q->model,
                                       status,
                                       0,
                                       0,
                                       0,
                                       0,
                                       total_lat,
                                       target->provider,
                                       q->guardrail_act,
                                       0.0);
                int rv = aigate_write_json(q->rc, status, ubody, ulen);
                free(ubody);
                chat_req_cleanup(q);
                return rv;
            }
            free(ubody);
            break; /* urc != 0 or no body: fall through to the generic 502 */
        }
        free(ubody);

        if (ci + 1 < q->n_candidates) {
            failover_warn("sync failover", q->model, target, &q->candidates[ci + 1], status, urc);
            continue;
        }
    }

    if (q->rc->set_header != NULL) {
        q->rc->set_header(q->rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(q->rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           PIPE_UPSTREAM,
                           0,
                           0,
                           0,
                           0,
                           total_lat,
                           last_provider,
                           q->guardrail_act,
                           0.0);
    chat_req_cleanup(q);
    return 0;
}

/** @brief Streaming chat failover loop (SSE + cache accumulation). */
/** @brief Handle a streaming attempt that ended before headers were sent.
 *
 *  Records circuit-breaker failure, releases bridge/accumulator, then either surfaces
 *  the upstream 4xx body, asks to retry the next candidate, or falls through to the
 *  generic 502. Always consumes `sbody`.
 *  @return -1 caller must return *ret_rv; 1 retry next candidate; 0 fall through. */
static int
handle_stream_preheaders(chat_req_t*               q,
                         upstream_target_t*        target,
                         int                       ci,
                         const provider_adapter_t* adapter,
                         stream_bridge_t*          bridge,
                         stream_cache_acc_t*       acc,
                         char*                     sbody,
                         size_t                    slen,
                         int                       status,
                         int                       urc,
                         bool                      is_failover,
                         uint64_t                  total_lat,
                         int*                      ret_rv)
{
    cb_record_failure(q->ac->cb, q->model, target->endpoint, status);
    adapter->stream_bridge_free(bridge);
    if (acc->accum_content != NULL) {
        free(acc->accum_content);
    }
    free(acc->line_buf);
    acc->line_buf = NULL;

    if (!is_failover && status >= 400) {
        /* Pre-headers 4xx: surface the upstream's own error body
         * (mirrors the non-streaming passthrough). Only when a
         * body was actually received (urc == 0). */
        if (sbody != NULL && urc == 0) {
            record_usage_and_event(q->ac,
                                   q->krec.key_id,
                                   q->model,
                                   status,
                                   0,
                                   0,
                                   0,
                                   0,
                                   total_lat,
                                   target->provider,
                                   q->guardrail_act,
                                   0.0);
            *ret_rv = aigate_write_json(q->rc, status, sbody, slen);
            free(sbody);
            return -1;
        }
        free(sbody);
        return 0; /* no error body: fall through to the generic 502 */
    }

    if (ci + 1 < q->n_candidates) {
        failover_warn("streaming failover", q->model, target, &q->candidates[ci + 1], status, urc);
        free(sbody);
        return 1;
    }
    free(sbody);
    return 0;
}

/**
 * @brief Context wrapper around stream bridge feed callback for measuring time-to-first-token (TTFT).
 */
typedef struct stream_feed_wrapper {
    int (*real_feed)(void*       bridge,
                     const void* chunk,
                     size_t      len); /**< Underlying stream bridge feed callback. */
    void*              real_bridge;    /**< Underlying bridge instance. */
    latency_tracker_t* lt;             /**< Latency tracker instance (optional). */
    const char*        model;          /**< Target model name for metrics. */
    const char*        endpoint;       /**< Target upstream endpoint address. */
    uint64_t           t0;             /**< Timestamp when request was dispatched. */
    bool first_chunk_recorded;         /**< True if first non-empty chunk has been observed. */
} stream_feed_wrapper_t;

static int
stream_feed_wrapper_fn(void* ctx, const void* chunk, size_t len)
{
    stream_feed_wrapper_t* w = (stream_feed_wrapper_t*)ctx;
    /* Step 1: Detect the first non-empty token chunk in the response stream */
    if (!w->first_chunk_recorded && len > 0) {
        w->first_chunk_recorded = true;
        /* Step 2: Calculate elapsed Time To First Token (TTFT) via monotonic clock */
        uint64_t ttft_ns = mono_ns() - w->t0;
        /* Step 3: Record TTFT sample in latency tracker for adaptive routing and P95 scoring */
        if (w->lt != NULL && ttft_ns > 0) {
            latency_tracker_record(w->lt, w->model, w->endpoint, ttft_ns);
        }
    }
    /* Step 4: Transparently forward chunk to underlying stream bridge without delay */
    return w->real_feed(w->real_bridge, chunk, len);
}

/**
 * @brief Handle streaming (SSE) /v1/chat/completions request execution.
 * @param[in,out] q Request processing context.
 * @return 0 on success, non-zero on error.
 */
int
handle_chat_stream(chat_req_t* q)
{
    uint64_t    total_lat = 0;
    const char* last_provider = q->route.provider;

    for (int ci = 0; ci < q->n_candidates; ci++) {
        upstream_target_t*        target = &q->candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_chat == NULL || adapter->stream_bridge_new == NULL) {
            continue;
        }

        model_rec_t cur_route = q->route;
        fill_cur_route(&q->route, target, &cur_route);
        last_provider = target->provider;

        char        url[1024];
        char*       merged = NULL;
        size_t      mlen = 0;
        const char* extra_hdrs[4][2] = {{0}};
        int         n_extra_hdrs = 0;

        if (adapter->build_chat(&cur_route,
                                q->eff_body != NULL ? (const char*)q->eff_body : NULL,
                                url,
                                sizeof url,
                                extra_hdrs,
                                &n_extra_hdrs,
                                &merged,
                                &mlen) != 0) {
            free(merged);
            continue;
        }

        stream_cache_acc_t acc;
        memset(&acc, 0, sizeof(acc));
        acc.orig_rc = q->rc;

        aigate_response_ctx proxy_rc = *q->rc;
        proxy_rc.impl = &acc;
        proxy_rc.set_header = stream_cache_acc_set_header;
        proxy_rc.write = stream_cache_acc_write;

        stream_bridge_t* bridge = adapter->stream_bridge_new(&proxy_rc, q->model);
        if (bridge == NULL) {
            free(merged);
            continue;
        }

        int      status = 0;
        char*    sbody = NULL;
        size_t   slen = 0;
        uint64_t t0 = mono_ns();
        long     silence_timeout_ms =
            q->ac->default_timeout_ms > 0 ? (long)q->ac->default_timeout_ms : 30000L;

        stream_feed_wrapper_t feed_wrapper = {
            .real_feed = adapter->stream_bridge_feed,
            .real_bridge = bridge,
            .lt = q->ac->lt,
            .model = q->model,
            .endpoint = target->endpoint,
            .t0 = t0,
            .first_chunk_recorded = false,
        };

        int  urc = upstream_stream_call(url,
                                        cur_route.upstream_key,
                                        extra_hdrs,
                                        n_extra_hdrs,
                                        merged,
                                        mlen,
                                        silence_timeout_ms,
                                        (upstream_chunk_fn)stream_feed_wrapper_fn,
                                        &feed_wrapper,
                                        &status,
                                        &sbody,
                                        &slen);
        bool headers_sent = adapter->stream_bridge_headers_sent(bridge);

        if (q->n_candidates == 1 && !headers_sent && (urc != 0 || status >= 500)) {
            struct timespec sl = {0, 200 * 1000000}; /* 200ms */
            nanosleep(&sl, NULL);
            free(sbody); /* the retry re-captures into the same pointers */
            sbody = NULL;
            feed_wrapper.t0 = mono_ns();
            feed_wrapper.first_chunk_recorded = false;
            urc = upstream_stream_call(url,
                                       cur_route.upstream_key,
                                       extra_hdrs,
                                       n_extra_hdrs,
                                       merged,
                                       mlen,
                                       silence_timeout_ms,
                                       (upstream_chunk_fn)stream_feed_wrapper_fn,
                                       &feed_wrapper,
                                       &status,
                                       &sbody,
                                       &slen);
            headers_sent = adapter->stream_bridge_headers_sent(bridge);
        }
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        if (q->ac->lt != NULL && lat > 0) {
            latency_tracker_record(q->ac->lt, q->model, target->endpoint, lat);
        }
        free(merged);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));

        if (!headers_sent) {
            int ph_rv = 0;
            int ph = handle_stream_preheaders(q,
                                              target,
                                              ci,
                                              adapter,
                                              bridge,
                                              &acc,
                                              sbody,
                                              slen,
                                              status,
                                              urc,
                                              is_failover,
                                              total_lat,
                                              &ph_rv);
            if (ph < 0) {
                chat_req_cleanup(q);
                return ph_rv;
            }
            if (ph > 0) {
                continue;
            }
            break;
        }

        long ptok = 0, ctok = 0, cached_tok = 0;
        adapter->stream_bridge_get_tokens(bridge, &ptok, &ctok, &cached_tok);

        if (urc != 0) {
            const char* err_msg = (urc == -110) ? "stream interrupted: silence timeout"
                                                : "stream interrupted: transport error";
            char        sse_err[256];
            snprintf(sse_err,
                     sizeof sse_err,
                     "data: "
                     "{\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\n"
                     "data: [DONE]\n\n",
                     err_msg);
            if (q->rc->write != NULL) {
                q->rc->write(q->rc->impl, sse_err, strlen(sse_err), true);
            }
            double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
            settle_success(
                q, PIPE_UPSTREAM, ptok, ctok, cached_tok, total_lat, target->provider, req_cost);
            adapter->stream_bridge_free(bridge);
            if (acc.accum_content != NULL) {
                free(acc.accum_content);
            }
            free(acc.line_buf);
            acc.line_buf = NULL;
            chat_req_cleanup(q);
            return 0;
        }

        cb_record_success(q->ac->cb, q->model, target->endpoint);
        adapter->stream_bridge_finish(bridge);
        double req_cost = calc_req_cost(&q->route, ptok, ctok, cached_tok);
        record_usage_and_event(q->ac,
                               q->krec.key_id,
                               q->model,
                               status > 0 ? status : 200,
                               ptok,
                               ctok,
                               cached_tok,
                               0,
                               total_lat,
                               target->provider,
                               q->guardrail_act,
                               req_cost);
        if (q->ac->be != NULL) {
            budget_enforce_record(
                q->ac->be, q->krec.key_id, q->krec.group_id, req_cost, ptok + ctok);
        }
        rl_reserve_tokens(q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);

        cache_store_stream(q, &acc, ptok, ctok, status, req_cost);
        if (acc.accum_content != NULL) {
            free(acc.accum_content);
        }
        free(acc.line_buf);
        acc.line_buf = NULL;
        adapter->stream_bridge_free(bridge);

        chat_req_cleanup(q);
        return 0;
    }

    if (q->rc->set_header != NULL) {
        q->rc->set_header(q->rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(q->rc, PIPE_UPSTREAM, "upstream_error", "upstream request failed");
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           PIPE_UPSTREAM,
                           0,
                           0,
                           0,
                           0,
                           total_lat,
                           last_provider,
                           q->guardrail_act,
                           0.0);
    chat_req_cleanup(q);
    return 0;
}
