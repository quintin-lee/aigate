/** @file pipeline_embeddings.c
 *  @brief /v1/embeddings pipeline handling (OpenAI & Gemini embeddings).
 */
#include "aigate_core_internal.h"
#include "metrics.h"
#include "provider_adapter.h"
#include "upstream_client.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Handle /v1/embeddings endpoint execution.
 * @param[in,out] q Request processing context.
 * @return 0 if handled or path bypassed, non-zero on error.
 */
int
handle_embeddings(chat_req_t* q)
{
    if (q->rq->path == NULL || strcmp(q->rq->path, "/v1/embeddings") != 0) {
        return 0;
    }
    int n_supported = 0;
    for (int ci = 0; ci < q->n_candidates; ci++) {
        const provider_adapter_t* adapter = provider_find(q->candidates[ci].provider);
        if (adapter != NULL && adapter->build_embeddings != NULL &&
            adapter->parse_embeddings_response != NULL) {
            n_supported++;
        }
    }
    if (n_supported == 0) {
        aigate_write_error(
            q->rc, 400, "unsupported_endpoint", "model or provider does not support embeddings");
        chat_req_cleanup(q);
        return 0;
    }

    uint64_t    total_lat = 0;
    const char* last_provider = q->route.provider;
    int         concurrency_saturated_count = 0;

    for (int ci = 0; ci < q->n_candidates; ci++) {
        upstream_target_t*        target = &q->candidates[ci];
        const provider_adapter_t* adapter = provider_find(target->provider);
        if (adapter == NULL || adapter->build_embeddings == NULL ||
            adapter->parse_embeddings_response == NULL) {
            continue;
        }

        if (model_router_acquire_target(target) != 0) {
            concurrency_saturated_count++;
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

        if (adapter->build_embeddings(&cur_route,
                                      q->eff_body != NULL ? (const char*)q->eff_body : NULL,
                                      url,
                                      sizeof url,
                                      extra_hdrs,
                                      &n_extra_hdrs,
                                      &merged,
                                      &mlen) != 0) {
            free(merged);
            model_router_release_target(target);
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
        model_router_release_target(target);
        uint64_t lat = mono_ns() - t0;
        total_lat += lat;
        free(merged);

        bool is_failover = (urc != 0 || status == 429 || (status >= 500 && status <= 504));
        if (!is_failover && status < 400) {
            cb_record_success(q->ac->cb, q->model, target->endpoint);
            char*  parsed_body = NULL;
            size_t parsed_len = 0;
            long   ptok = 0;
            int    parsed_status = status;
            if (adapter->parse_embeddings_response(ubody ? ubody : "",
                                                   ulen,
                                                   q->model,
                                                   &parsed_status,
                                                   &parsed_body,
                                                   &parsed_len,
                                                   &ptok) != 0) {
                free(ubody);
                aigate_write_error(
                    q->rc, 502, "upstream_error", "failed to parse upstream embeddings response");
                chat_req_cleanup(q);
                return 0;
            }
            free(ubody);

            double req_cost = calc_req_cost(&q->route, ptok, 0, 0);
            settle_success(q, parsed_status, ptok, 0, 0, total_lat, target->provider, req_cost);

            int rv =
                aigate_write_json(q->rc, parsed_status, parsed_body ? parsed_body : "", parsed_len);
            free(parsed_body);
            chat_req_cleanup(q);
            return rv;
        }

        cb_record_failure(q->ac->cb, q->model, target->endpoint, status);

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
                                       target->provider,
                                       q->guardrail_act,
                                       0.0);
                int rv = aigate_write_json(q->rc, status, ubody, ulen);
                free(ubody);
                chat_req_cleanup(q);
                return rv;
            }
            free(ubody);
            break;
        }
        free(ubody);

        if (ci + 1 < q->n_candidates) {
            failover_warn(
                "failover embeddings", q->model, target, &q->candidates[ci + 1], status, urc);
            continue;
        }
    }

    if (concurrency_saturated_count > 0 && concurrency_saturated_count == q->n_candidates) {
        if (q->rc->set_header != NULL) {
            q->rc->set_header(q->rc->impl, "Retry-After", "1");
        }
        aigate_write_error(q->rc, 429, "rate_limit_error", "upstream concurrency limit exceeded");
        chat_req_cleanup(q);
        return 0;
    }

    if (q->rc->set_header != NULL) {
        q->rc->set_header(q->rc->impl, "X-Upstream-Provider", last_provider);
    }
    aigate_write_error(
        q->rc, PIPE_UPSTREAM, "upstream_error", "upstream embeddings request failed");
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
