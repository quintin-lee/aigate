/** @file aigate_core.c
 *  @brief Pipeline implementation (see aigate_core.h). */
#include "aigate_core.h"
#include "aigate_core_internal.h"
#include "aigate_log.h"
#include "metrics.h"
#include "model_router.h"
#include "provider_adapter.h"
#include "upstream_client.h"
#include "health_prober.h"
#include "event_bus.h"
#include "response_cache.h"
#include "filter_chain.h"
#include "latency_tracker.h"

#include <inttypes.h>
#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "store/pg_store.h"

/** @brief Current monotonic time in nanoseconds, for latency measurement (immune to system clock jumps).
 *  @return Nanoseconds since CLOCK_MONOTONIC epoch. */
uint64_t
mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int
aigate_core_reload_guardrails(aigate_core* ac)
{
    if (ac == NULL) {
        return -1;
    }
    guardrails_ctx_t* new_gr = guardrails_create();
    if (new_gr == NULL) {
        return -1;
    }
    if (ac->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(ac->ps);
        if (ops != NULL && ops->list_guardrails_rules != NULL) {
            guardrail_rule_t rules[256];
            int              n_rules = 0;
            if (ops->list_guardrails_rules(ops->ctx, rules, 256, &n_rules) == 0 && n_rules > 0) {
                guardrails_load_rules(new_gr, rules, (size_t)n_rules);
            }
        }
    }
    guardrails_ctx_t* old_gr = ac->gr;
    ac->gr = new_gr;
    if (old_gr != NULL) {
        guardrails_destroy(old_gr);
    }
    return 0;
}

int
aigate_core_reload_shadow_rules(aigate_core* ac)
{
    if (ac == NULL) {
        return -1;
    }
    pthread_mutex_lock(&ac->shadow_rules_lock);
    ac->n_shadow_rules = 0;
    if (ac->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(ac->ps);
        if (ops != NULL && ops->list_shadow_rules != NULL) {
            ops->list_shadow_rules(ops->ctx, ac->shadow_rules, 128, &ac->n_shadow_rules);
        }
    }
    pthread_mutex_unlock(&ac->shadow_rules_lock);
    return 0;
}

int
aigate_core_reload_compressor_rules(aigate_core* ac)
{
    if (ac == NULL) {
        return -1;
    }
    pthread_mutex_lock(&ac->comp_rules_lock);
    ac->n_comp_rules = 0;
    if (ac->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(ac->ps);
        if (ops != NULL && ops->list_compressor_rules != NULL) {
            ops->list_compressor_rules(ops->ctx, ac->comp_rules, 64, &ac->n_comp_rules);
        }
    }
    pthread_mutex_unlock(&ac->comp_rules_lock);
    return 0;
}

int
aigate_core_reload_cache_optimizer_rules(aigate_core* ac)
{
    if (ac == NULL) {
        return -1;
    }
    pthread_mutex_lock(&ac->cache_opt_rules_lock);
    ac->n_cache_opt_rules = 0;
    if (ac->ps != NULL) {
        const pg_ops_t* ops = pg_store_ops(ac->ps);
        if (ops != NULL && ops->list_cache_optimizer_rules != NULL) {
            ops->list_cache_optimizer_rules(
                ops->ctx, ac->cache_opt_rules, 64, &ac->n_cache_opt_rules);
        }
    }
    pthread_mutex_unlock(&ac->cache_opt_rules_lock);
    return 0;
}

/** @brief Extract prompt preview snippet from parsed JSON body. */
static void
extract_prompt_snippet(json_t* jbody, char* out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (jbody == NULL || !json_is_object(jbody)) {
        return;
    }
    json_t* messages = json_object_get(jbody, "messages");
    if (messages != NULL && json_is_array(messages)) {
        size_t n = json_array_size(messages);
        for (size_t i = n; i > 0; i--) {
            json_t* m = json_array_get(messages, i - 1);
            if (m != NULL && json_is_object(m)) {
                json_t* jcontent = json_object_get(m, "content");
                if (jcontent != NULL && json_is_string(jcontent)) {
                    snprintf(out, out_sz, "%s", json_string_value(jcontent));
                    return;
                }
            }
        }
    }
    json_t* prompt = json_object_get(jbody, "prompt");
    if (prompt != NULL && json_is_string(prompt)) {
        snprintf(out, out_sz, "%s", json_string_value(prompt));
    }
}

/** @brief Estimate request cost from route pricing (pricing_json in_mtok/out_mtok per MTok).
 *  @param route      Matched route record; NULL or missing pricing = free.
 *  @param prompt     Total input tokens; @param completion output tokens.
 *  @param cached     Cache-hit tokens among input, priced at cached_mtok_discount (default 1.0).
 *  @return Cost = (unhit_input*in_price + cached_input*in_price*discount + output*out_price) / 1e6; returns 0.0 if pricing missing/invalid. */
double
calc_req_cost(const model_rec_t* route, long prompt, long completion, long cached)
{
    if (route == NULL || route->pricing_json[0] == '\0') {
        return 0.0;
    }
    json_error_t jerr;
    json_t*      jp = json_loads(route->pricing_json, 0, &jerr);
    if (jp == NULL || !json_is_object(jp)) {
        if (jp != NULL) {
            json_decref(jp);
        }
        return 0.0;
    }
    json_t* jin = json_object_get(jp, "in_mtok");
    json_t* jout = json_object_get(jp, "out_mtok");
    if (jin == NULL || jout == NULL || !json_is_number(jin) || !json_is_number(jout)) {
        json_decref(jp);
        return 0.0;
    }
    double  in_mtok = json_number_value(jin);
    double  out_mtok = json_number_value(jout);
    json_t* jdisc = json_object_get(jp, "cached_mtok_discount");
    double  cached_discount =
        (jdisc != NULL && json_is_number(jdisc)) ? json_number_value(jdisc) : 1.0;
    json_decref(jp);

    long unhit_prompt = (prompt >= cached) ? (prompt - cached) : 0;
    return ((double)unhit_prompt * in_mtok + (double)cached * in_mtok * cached_discount +
            (double)completion * out_mtok) /
           1000000.0;
}

double
calc_cache_savings(const model_rec_t* route, long cached)
{
    if (route == NULL || cached <= 0 || route->pricing_json[0] == '\0') {
        return 0.0;
    }
    json_error_t jerr;
    json_t*      jp = json_loads(route->pricing_json, 0, &jerr);
    if (jp == NULL || !json_is_object(jp)) {
        if (jp != NULL) {
            json_decref(jp);
        }
        return 0.0;
    }
    json_t* jin = json_object_get(jp, "in_mtok");
    if (jin == NULL || !json_is_number(jin)) {
        json_decref(jp);
        return 0.0;
    }
    double  in_mtok = json_number_value(jin);
    json_t* jdisc = json_object_get(jp, "cached_mtok_discount");
    double  cached_discount =
        (jdisc != NULL && json_is_number(jdisc)) ? json_number_value(jdisc) : 0.50;
    json_decref(jp);

    if (cached_discount > 1.0) {
        cached_discount = 1.0;
    }
    if (cached_discount < 0.0) {
        cached_discount = 0.0;
    }

    return ((double)cached * in_mtok * (1.0 - cached_discount)) / 1000000.0;
}

/**
 * @brief Record one request: usage meter (um_record_full) + request event (event_bus_publish_request).
 * @param[in] ac            Core instance (returns immediately if NULL).
 * @param[in] key_id        Authenticated key ID.
 * @param[in] model         Model identifier string.
 * @param[in] status        HTTP status code.
 * @param[in] ptok          Prompt tokens count.
 * @param[in] ctok          Completion tokens count.
 * @param[in] cached_tok    Cached prompt tokens count.
 * @param[in] reasoning_tok Reasoning tokens count.
 * @param[in] lat_ns        Latency in nanoseconds.
 * @param[in] provider      Target provider name.
 * @param[in] guardrail_act Guardrail action string.
 * @param[in] req_cost      Estimated cost in USD.
 */
void
record_usage_and_event(aigate_core* ac,
                       long         key_id,
                       const char*  model,
                       int          status,
                       long         ptok,
                       long         ctok,
                       long         cached_tok,
                       long         reasoning_tok,
                       uint64_t     lat_ns,
                       const char*  provider,
                       const char*  guardrail_act,
                       double       req_cost)
{
    if (ac == NULL) {
        return;
    }
    if (ac->um != NULL) {
        um_record_full(ac->um,
                       key_id,
                       model,
                       status,
                       ptok,
                       ctok,
                       cached_tok,
                       reasoning_tok,
                       lat_ns,
                       provider,
                       guardrail_act);
    }
    if (ac->eb != NULL) {
        event_bus_publish_request(
            ac->eb, key_id, model, provider, status, lat_ns, ptok, ctok, req_cost, guardrail_act);
    }
}

void
aigate_record_audit_ex(aigate_core*     ac,
                       const char*      trace_id,
                       const char*      client_ip,
                       int64_t          key_id,
                       const char*      model,
                       const char*      routed_model,
                       const char*      provider,
                       int              http_status,
                       uint32_t         prompt_tokens,
                       uint32_t         completion_tokens,
                       uint64_t         latency_ns,
                       uint64_t         ttft_ns,
                       audit_severity_t severity,
                       const char*      violation_type,
                       const char*      rule_detail,
                       const char*      fallback_reason,
                       const char*      prompt_raw,
                       size_t           prompt_len)
{
    if (ac == NULL) {
        return;
    }

    if (ac->audit != NULL) {
        audit_event_t ev;
        audit_event_init(&ev);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ev.timestamp_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);

        if (trace_id != NULL) {
            snprintf(ev.trace_id, sizeof(ev.trace_id), "%s", trace_id);
        }
        if (client_ip != NULL) {
            snprintf(ev.client_ip, sizeof(ev.client_ip), "%s", client_ip);
        }
        ev.key_id = key_id;
        if (model != NULL) {
            snprintf(ev.model, sizeof(ev.model), "%s", model);
        }
        if (routed_model != NULL && routed_model[0] != '\0') {
            snprintf(ev.routed_model, sizeof(ev.routed_model), "%s", routed_model);
        } else if (model != NULL) {
            snprintf(ev.routed_model, sizeof(ev.routed_model), "%s", model);
        }
        if (provider != NULL) {
            snprintf(ev.provider, sizeof(ev.provider), "%s", provider);
        }
        ev.http_status = http_status;
        ev.prompt_tokens = prompt_tokens;
        ev.completion_tokens = completion_tokens;
        ev.latency_ns = latency_ns;
        ev.ttft_ns = ttft_ns;
        ev.severity = severity;
        if (violation_type != NULL) {
            snprintf(ev.violation_type, sizeof(ev.violation_type), "%s", violation_type);
        }
        if (rule_detail != NULL) {
            snprintf(ev.rule_detail, sizeof(ev.rule_detail), "%s", rule_detail);
        }
        if (fallback_reason != NULL) {
            snprintf(ev.fallback_reason, sizeof(ev.fallback_reason), "%s", fallback_reason);
        }

        if (severity >= AUDIT_SEV_VIOLATION && prompt_raw != NULL && prompt_len > 0) {
            int    max_len = audit_logger_get_max_prompt_len(ac->audit);
            size_t limit = (prompt_len > (size_t)max_len) ? (size_t)max_len : prompt_len;
            audit_event_set_prompt(&ev, prompt_raw, limit);
        }

        audit_logger_record(ac->audit, &ev);
        audit_event_cleanup(&ev);
    }

    /* Persist violations to cold tier in PostgreSQL if available */
    if (severity >= AUDIT_SEV_VIOLATION && ac->ps != NULL) {
        audit_violation_record_t vrec;
        memset(&vrec, 0, sizeof(vrec));
        snprintf(vrec.trace_id, sizeof(vrec.trace_id), "%s", trace_id ? trace_id : "");
        snprintf(vrec.tenant_id, sizeof(vrec.tenant_id), "%s", "default");
        if (key_id > 0) {
            snprintf(vrec.tenant_id, sizeof(vrec.tenant_id), "key#%" PRId64, key_id);
        }
        snprintf(vrec.client_ip, sizeof(vrec.client_ip), "%s", client_ip ? client_ip : "");
        snprintf(vrec.model, sizeof(vrec.model), "%s", model ? model : "");
        snprintf(vrec.routed_model,
                 sizeof(vrec.routed_model),
                 "%s",
                 routed_model ? routed_model : (model ? model : ""));
        snprintf(vrec.severity, sizeof(vrec.severity), "%s", audit_severity_str(severity));
        snprintf(vrec.rule_tag,
                 sizeof(vrec.rule_tag),
                 "%s",
                 violation_type ? violation_type : "VIOLATION");
        vrec.http_status = http_status;
        vrec.ttft_ms = (uint32_t)(ttft_ns / 1000000ULL);
        vrec.total_latency_ms = (uint32_t)(latency_ns / 1000000ULL);
        snprintf(vrec.fallback_reason,
                 sizeof(vrec.fallback_reason),
                 "%s",
                 fallback_reason ? fallback_reason : "");
        if (prompt_raw != NULL && prompt_len > 0) {
            vrec.prompt_snapshot = (char*)malloc(prompt_len + 1);
            if (vrec.prompt_snapshot != NULL) {
                memcpy(vrec.prompt_snapshot, prompt_raw, prompt_len);
                vrec.prompt_snapshot[prompt_len] = '\0';
            }
        }
        if (rule_detail != NULL && rule_detail[0] != '\0') {
            vrec.completion_snapshot = (char*)strdup(rule_detail);
        }
        pg_store_insert_audit_violation(ac->ps, &vrec);
        if (vrec.prompt_snapshot != NULL) {
            free(vrec.prompt_snapshot);
        }
        if (vrec.completion_snapshot != NULL) {
            free(vrec.completion_snapshot);
        }
    }
}

void
aigate_record_audit(aigate_core*     ac,
                    const char*      trace_id,
                    const char*      client_ip,
                    int64_t          key_id,
                    const char*      model,
                    const char*      provider,
                    int              http_status,
                    uint32_t         prompt_tokens,
                    uint32_t         completion_tokens,
                    uint64_t         latency_ns,
                    uint64_t         ttft_ns,
                    audit_severity_t severity,
                    const char*      violation_type,
                    const char*      rule_detail,
                    const char*      prompt_raw,
                    size_t           prompt_len)
{
    aigate_record_audit_ex(ac,
                           trace_id,
                           client_ip,
                           key_id,
                           model,
                           model,
                           provider,
                           http_status,
                           prompt_tokens,
                           completion_tokens,
                           latency_ns,
                           ttft_ns,
                           severity,
                           violation_type,
                           rule_detail,
                           "",
                           prompt_raw,
                           prompt_len);
}

int
aigate_core_init(aigate_core*   ac,
                 pg_store_t*    ps,
                 const uint8_t* master32,
                 int            default_timeout_ms,
                 int            flush_interval_s)
{
    memset(ac, 0, sizeof *ac);
    ac->tracer_cfg.enabled = true;
    ac->tracer_cfg.sample_rate = 1.0;
    ac->tracer_cfg.slow_threshold_ms = 2000;
    ac->tracer_cfg.otlp_endpoint[0] = '\0';
    ac->trace_rb = trace_ring_buffer_create(TRACE_RING_BUFFER_DEFAULT_CAPACITY);

    if (auth_key_init(&ac->keys, ps) != 0) {
        if (ac->trace_rb != NULL) {
            trace_ring_buffer_destroy(ac->trace_rb);
            ac->trace_rb = NULL;
        }
        return -1;
    }
    ac->rl = ratelimit_new();
    ac->router = model_router_new(ps, master32);
    ac->um = usage_meter_new(ps, ac->rl, flush_interval_s);
    ac->cb = cb_create();
    ac->ps = ps;
    ac->default_timeout_ms = default_timeout_ms;
    aigate_core_reload_guardrails(ac);
    ac->be = budget_enforce_create(ps, NULL);
    if (ac->be != NULL) {
        budget_enforce_init_from_db(ac->be);
    }
    ac->eb = event_bus_new();
    if (ac->cb != NULL && ac->eb != NULL) {
        cb_set_event_bus(ac->cb, ac->eb);
    }
    if (ac->be != NULL && ac->eb != NULL) {
        budget_enforce_set_event_bus(ac->be, ac->eb);
    }
    ac->hp = health_prober_new(ps, master32, ac->eb, 60);
    ac->rc = response_cache_new(0, 0, 0);
    if (ac->rc != NULL) {
        AIGATE_LOG_INFO("response cache initialized (16 shards, 128MB, 20000 entries max)");
    } else {
        AIGATE_LOG_WARN("failed to initialize response cache");
    }
    ac->lt = latency_tracker_create();

    pthread_mutex_init(&ac->shadow_rules_lock, NULL);
    ac->shadow_eng =
        shadow_engine_create(ps, SHADOW_QUEUE_DEFAULT_CAPACITY, SHADOW_EVAL_DEFAULT_CAPACITY);
    if (ac->shadow_eng != NULL) {
        shadow_engine_start(ac->shadow_eng);
    }
    aigate_core_reload_shadow_rules(ac);

    pthread_mutex_init(&ac->comp_rules_lock, NULL);
    ac->comp_cache = compressor_cache_create(200);
    aigate_core_reload_compressor_rules(ac);

    pthread_mutex_init(&ac->cache_opt_rules_lock, NULL);
    ac->cache_opt_cache = cache_optimizer_cache_create(200);
    aigate_core_reload_cache_optimizer_rules(ac);

    if (ac->rl == NULL || ac->router == NULL || ac->um == NULL || ac->cb == NULL) {
        if (ac->cache_opt_cache != NULL) {
            cache_optimizer_cache_destroy(ac->cache_opt_cache);
            ac->cache_opt_cache = NULL;
        }
        pthread_mutex_destroy(&ac->cache_opt_rules_lock);
        if (ac->comp_cache != NULL) {
            compressor_cache_destroy(ac->comp_cache);
            ac->comp_cache = NULL;
        }
        pthread_mutex_destroy(&ac->comp_rules_lock);
        if (ac->shadow_eng != NULL) {
            shadow_engine_destroy(ac->shadow_eng);
            ac->shadow_eng = NULL;
        }
        pthread_mutex_destroy(&ac->shadow_rules_lock);
        if (ac->tm != NULL) {
            tracer_manager_destroy(ac->tm);
            ac->tm = NULL;
        }
        if (ac->trace_rb != NULL) {
            trace_ring_buffer_destroy(ac->trace_rb);
            ac->trace_rb = NULL;
        }
        if (ac->lt != NULL) {
            latency_tracker_destroy(ac->lt);
        }
        if (ac->rc != NULL) {
            response_cache_free(ac->rc);
        }
        if (ac->hp != NULL) {
            health_prober_free(ac->hp);
        }
        if (ac->eb != NULL) {
            event_bus_free(ac->eb);
        }
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
        if (ac->gr != NULL) {
            guardrails_destroy(ac->gr);
        }
        if (ac->be != NULL) {
            budget_enforce_destroy(ac->be);
        }
        auth_key_shutdown(&ac->keys);
        memset(ac, 0, sizeof *ac);
        return -1;
    }

    if (ac->trace_rb != NULL) {
        ac->tm = tracer_manager_create(&ac->tracer_cfg, ac->trace_rb);
    }
    return 0;
}

void
aigate_core_shutdown(aigate_core* ac)
{
    if (ac == NULL) {
        return;
    }
    if (ac->tm != NULL) {
        tracer_manager_destroy(ac->tm);
        ac->tm = NULL;
    }
    if (ac->trace_rb != NULL) {
        trace_ring_buffer_destroy(ac->trace_rb);
        ac->trace_rb = NULL;
    }
    if (ac->lt != NULL) {
        latency_tracker_destroy(ac->lt);
        ac->lt = NULL;
    }
    if (ac->rc != NULL) {
        response_cache_free(ac->rc);
        ac->rc = NULL;
        AIGATE_LOG_INFO("response cache shut down");
    }
    if (ac->hp != NULL) {
        health_prober_free(ac->hp);
        ac->hp = NULL;
    }
    if (ac->eb != NULL) {
        event_bus_free(ac->eb);
        ac->eb = NULL;
    }
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
    if (ac->gr != NULL) {
        guardrails_destroy(ac->gr);
        ac->gr = NULL;
    }
    if (ac->be != NULL) {
        budget_enforce_destroy(ac->be);
        ac->be = NULL;
    }
    if (ac->shadow_eng != NULL) {
        shadow_engine_destroy(ac->shadow_eng);
        ac->shadow_eng = NULL;
    }
    pthread_mutex_destroy(&ac->shadow_rules_lock);
    if (ac->comp_cache != NULL) {
        compressor_cache_destroy(ac->comp_cache);
        ac->comp_cache = NULL;
    }
    pthread_mutex_destroy(&ac->comp_rules_lock);
    if (ac->cache_opt_cache != NULL) {
        cache_optimizer_cache_destroy(ac->cache_opt_cache);
        ac->cache_opt_cache = NULL;
    }
    pthread_mutex_destroy(&ac->cache_opt_rules_lock);
    if (ac->audit != NULL) {
        audit_logger_destroy(ac->audit);
        ac->audit = NULL;
    }
    auth_key_shutdown(&ac->keys);
}

int
aigate_write_json(aigate_response_ctx* rc, int status, const char* body, size_t len)
{
    if (rc == NULL) {
        return -1;
    }
    if (!rc->headers_sent) {
        rc->status = status;
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "Content-Type", "application/json");
            char cl[32];
            snprintf(cl, sizeof cl, "%zu", len);
            rc->set_header(rc->impl, "Content-Length", cl);
        }
        rc->headers_sent = true;
    }
    if (rc->write != NULL) {
        return rc->write(rc->impl, body, len, true);
    }
    return 0;
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

int
aigate_write_anthropic_error(aigate_response_ctx* rc,
                             int                  http_status,
                             const char*          type,
                             const char*          message)
{
    json_t* err = json_object();
    json_object_set_new(err, "type", json_string(type ? type : "api_error"));
    json_object_set_new(err, "message", json_string(message ? message : ""));
    json_t* root = json_object();
    json_object_set_new(root, "type", json_string("error"));
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

int
aigate_write_gemini_error(aigate_response_ctx* rc,
                          int                  http_status,
                          const char*          status_str,
                          const char*          message)
{
    json_t* err = json_object();
    json_object_set_new(err, "code", json_integer(http_status));
    json_object_set_new(err, "message", json_string(message ? message : ""));
    json_object_set_new(err, "status", json_string(status_str ? status_str : "UNKNOWN"));
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

void
aigate_inject_cache_optimizer_headers(const chat_req_t* chatq, void* conn)
{
    (void)conn;
    if (chatq == NULL || chatq->rc == NULL || chatq->rc->set_header == NULL) {
        return;
    }
    const char* hit_str = (chatq->upstream_cached_tokens > 0) ? "true" : "false";
    chatq->rc->set_header(chatq->rc->impl, "X-Aigate-Prompt-Cache-Hit", hit_str);

    char buf[64];
    snprintf(buf, sizeof(buf), "%u", chatq->upstream_cached_tokens);
    chatq->rc->set_header(chatq->rc->impl, "X-Aigate-Prompt-Cache-Tokens", buf);

    snprintf(buf, sizeof(buf), "%.4f", chatq->upstream_cache_savings_usd);
    chatq->rc->set_header(chatq->rc->impl, "X-Aigate-Prompt-Cache-Savings", buf);
}

/** @brief Release owned request resources (mirrors the historical triple-cleanup). */
void
chat_req_cleanup(chat_req_t* q)
{
    if (q == NULL) {
        return;
    }

    if (q->trace_ctx.trace_id[0] != '\0') {
        int           status_code = (q->rc != NULL && q->rc->status != 0) ? q->rc->status : 200;
        span_status_t span_status = (status_code >= 400) ? SPAN_STATUS_ERROR : SPAN_STATUS_OK;
        tracer_span_end(
            &q->trace_ctx, "root", span_status, (status_code >= 400) ? "request error" : NULL);

        uint64_t elapsed_ms = 0;
        for (int i = 0; i < q->trace_ctx.span_count; i++) {
            if (strcmp(q->trace_ctx.spans[i].name, "root") == 0) {
                if (q->trace_ctx.spans[i].end_time_ns >= q->trace_ctx.spans[i].start_time_ns) {
                    elapsed_ms =
                        (q->trace_ctx.spans[i].end_time_ns - q->trace_ctx.spans[i].start_time_ns) /
                        1000000ULL;
                }
                break;
            }
        }

        if (q->ac != NULL) {
            bool sampled =
                tracer_should_sample(&q->trace_ctx, &q->ac->tracer_cfg, status_code, elapsed_ms);
            if (sampled && q->ac->trace_rb != NULL) {
                trace_ring_buffer_push(q->ac->trace_rb, &q->trace_ctx);
            }
        }
        tracer_context_cleanup(&q->trace_ctx);
    }

    if (q->has_shadow && q->ac != NULL && q->ac->shadow_eng != NULL) {
        int    status_code = (q->rc != NULL && q->rc->status != 0) ? q->rc->status : 200;
        double elapsed_ms = 40.0;
        shadow_engine_record_primary(q->ac->shadow_eng,
                                     q->eval_id,
                                     elapsed_ms,
                                     0.0,
                                     status_code,
                                     0,
                                     0.0,
                                     "Primary completed");
    }

    if (q->jbody != NULL) {
        json_decref(q->jbody);
        q->jbody = NULL;
    }
    key_rec_free(&q->krec);
    free(q->sanitized_body);
    q->sanitized_body = NULL;
    memset(&q->pii_map, 0, sizeof(q->pii_map));
    prompt_compressor_result_cleanup(&q->comp_result);
    cache_optimizer_result_cleanup(&q->cache_opt_result);
}

/** @brief Auth → QPS → daily quota → monthly budget gates.
 *  @return 0 when all gates pass; non-zero when an error was already written. */
int
gate_request(chat_req_t* q)
{
    aigate_core*         ac = q->ac;
    aigate_request_ctx*  rq = q->rq;
    aigate_response_ctx* rc = q->rc;

    tracer_span_start(
        &q->trace_ctx, "auth_and_limits", SPAN_KIND_INTERNAL, q->trace_ctx.root_span_id);

    /* --- auth --- */
    int arc = auth_key_resolve(&ac->keys, rq->bearer, &q->krec);
    if (arc != 0) {
        tracer_span_end(&q->trace_ctx, "auth_and_limits", SPAN_STATUS_ERROR, "invalid api key");
        aigate_write_error(rc, PIPE_AUTH, "auth_error", "invalid api key");
        return -1;
    }

    /* --- rate limit (before /v1/models and model routing so every data-plane
     *  request, including GET /v1/models, counts against the key's QPS) --- */
    long retry_ms = 0;
    int  rrc = rl_allow_request(ac->rl, q->krec.key_id, q->krec.rate_qps, &retry_ms);
    if (rrc != 0) {
        if (retry_ms == -1) {
            /* Redis fail-closed sentinel: distributed state unavailable → 503 */
            tracer_span_end(&q->trace_ctx,
                            "auth_and_limits",
                            SPAN_STATUS_ERROR,
                            "distributed_state_unavailable");
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
            return -1;
        }
        long ra_s = (retry_ms + 999) / 1000;
        if (ra_s < 1) {
            ra_s = 1;
        }
        char ra[32];
        snprintf(ra, sizeof ra, "%ld", ra_s);
        rc->set_header(rc->impl, "Retry-After", ra);
        tracer_span_end(&q->trace_ctx, "auth_and_limits", SPAN_STATUS_ERROR, "rate limit exceeded");
        aigate_write_error(rc, PIPE_RATE, "rate_limit", "rate limit exceeded");
        return -1;
    }
    /* --- daily token quota gate (after the QPS gate, before /v1/models so
     *  the limit applies uniformly to all data-plane traffic) --- */
    if (q->krec.daily_token_quota > 0) {
        long rem = rl_remaining_daily(ac->rl, q->krec.key_id, q->krec.daily_token_quota);
        if (rem == LONG_MIN) {
            /* Redis fail-closed sentinel: distributed state unavailable → 503 */
            tracer_span_end(&q->trace_ctx,
                            "auth_and_limits",
                            SPAN_STATUS_ERROR,
                            "distributed_state_unavailable");
            aigate_write_error(rc, 503, "server_error", "distributed_state_unavailable");
            return -1;
        }
        if (rem <= 0) {
            time_t now = time(NULL);
            time_t next = (time_t)(now - (now % 86400)) + 86400; /* next UTC midnight */
            char   ra[32];
            snprintf(ra, sizeof ra, "%ld", (long)(next - now));
            rc->set_header(rc->impl, "Retry-After", ra);
            tracer_span_end(
                &q->trace_ctx, "auth_and_limits", SPAN_STATUS_ERROR, "daily token quota exceeded");
            aigate_write_error(rc, PIPE_RATE, "daily_quota_exceeded", "daily token quota exceeded");
            return -1;
        }
    }

    /* --- monthly budget limit gate --- */
    if (ac->be != NULL) {
        char b_err[256] = {0};
        if (budget_enforce_check(ac->be,
                                 q->krec.key_id,
                                 q->krec.group_id,
                                 q->krec.monthly_cost_budget,
                                 q->krec.monthly_token_budget,
                                 0.0,
                                 b_err,
                                 sizeof b_err) != 0) {
            tracer_span_end(&q->trace_ctx,
                            "auth_and_limits",
                            SPAN_STATUS_ERROR,
                            b_err[0] ? b_err : "budget exceeded");
            aigate_write_error(rc,
                               PIPE_RATE,
                               "budget_exceeded",
                               b_err[0] ? b_err : "monthly budget limit exceeded");
            return -1;
        }
    }
    tracer_span_set_attr_int(&q->trace_ctx, "root", "aigate.client.key_id", q->krec.key_id);
    tracer_span_end(&q->trace_ctx, "auth_and_limits", SPAN_STATUS_OK, NULL);
    return 0;
}

/** @brief Body model parse → allowlist → router resolve → candidates → guardrails.
 *  @return 0 on success with q filled; non-zero when an error was already written. */
int
resolve_chat_target(chat_req_t* q)
{
    aigate_core*         ac = q->ac;
    aigate_request_ctx*  rq = q->rq;
    aigate_response_ctx* rc = q->rc;

    /* --- model + allowlist (parsed from request body) --- */
    json_t* jbody = NULL;
    if (rq->body != NULL && rq->body_len > 0) {
        jbody = json_loads((const char*)rq->body, 0, NULL);
    }
    q->jbody = jbody;
    q->model = "";
    if (jbody != NULL) {
        json_t* jm = json_object_get(jbody, "model");
        if (jm != NULL && json_is_string(jm)) {
            q->model = json_string_value(jm);
        }
    }
    if (q->model[0] == '\0' || !key_allows_model(&q->krec, q->model)) {
        aigate_write_error(rc, PIPE_FORBIDDEN, "auth_error", "model not allowed for this key");
        return -1;
    }
    snprintf(q->requested_model, sizeof(q->requested_model), "%s", q->model);

    /* --- canary routing evaluation --- */
    char eff_model[64] = {0};
    bool is_canary = false;
    long canary_rule_id = 0;
    pthread_mutex_lock(&ac->shadow_rules_lock);
    model_router_apply_canary(ac->router,
                              ac->cb,
                              ac->shadow_rules,
                              ac->n_shadow_rules,
                              q->model,
                              NULL,
                              eff_model,
                              sizeof(eff_model),
                              &is_canary,
                              &canary_rule_id);
    pthread_mutex_unlock(&ac->shadow_rules_lock);

    if (is_canary) {
        q->is_canary = true;
        q->canary_rule_id = canary_rule_id;
        snprintf(q->canary_model, sizeof(q->canary_model), "%s", eff_model);
        q->model = q->canary_model;
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-Aigate-Canary", "true");
        }
    }

    /* --- SLA soft degradation fallback evaluation --- */
    char sla_routed[64] = {0};
    char sla_reason[32] = {0};
    bool is_sla_fallback = false;
    if (model_router_resolve_with_sla(ac->cb,
                                      q->model,
                                      sla_routed,
                                      sizeof(sla_routed),
                                      &is_sla_fallback,
                                      sla_reason,
                                      sizeof(sla_reason)) == 0 &&
        is_sla_fallback) {
        q->is_fallback = true;
        snprintf(q->fallback_reason, sizeof(q->fallback_reason), "%s", sla_reason);
        snprintf(q->routed_model, sizeof(q->routed_model), "%s", sla_routed);
        q->model = q->routed_model;
        if (rc->set_header != NULL) {
            rc->set_header(rc->impl, "X-AIGate-Fallback", "true");
            rc->set_header(rc->impl, "X-AIGate-Fallback-Reason", q->fallback_reason);
            rc->set_header(rc->impl, "X-AIGate-Routed-Model", q->routed_model);
        }
    }

    /* --- traffic shadowing evaluation --- */
    if (!q->is_canary && ac->shadow_eng != NULL) {
        pthread_mutex_lock(&ac->shadow_rules_lock);
        for (int i = 0; i < ac->n_shadow_rules; i++) {
            const shadow_rule_t* r = &ac->shadow_rules[i];
            if (r->mode == TRAFFIC_MODE_SHADOW && r->enabled) {
                if (shadow_rule_matches(r, q->model, NULL) && shadow_rule_should_sample(r)) {
                    q->has_shadow = true;
                    q->shadow_rule = *r;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&ac->shadow_rules_lock);

        if (q->has_shadow) {
            shadow_generate_eval_id(q->eval_id, sizeof(q->eval_id));
            char prompt_preview[256] = {0};
            extract_prompt_snippet(q->jbody, prompt_preview, sizeof(prompt_preview));

            shadow_engine_start_pairing(ac->shadow_eng,
                                        q->eval_id,
                                        q->trace_ctx.trace_id,
                                        q->shadow_rule.source_model,
                                        q->shadow_rule.target_model,
                                        TRAFFIC_MODE_SHADOW,
                                        prompt_preview);

            shadow_task_t task;
            memset(&task, 0, sizeof(task));
            snprintf(task.eval_id, sizeof(task.eval_id), "%s", q->eval_id);
            snprintf(task.trace_id, sizeof(task.trace_id), "%s", q->trace_ctx.trace_id);
            task.rule = q->shadow_rule;
            if (rq->body != NULL && rq->body_len > 0) {
                task.body_copy = malloc(rq->body_len + 1);
                if (task.body_copy != NULL) {
                    memcpy(task.body_copy, rq->body, rq->body_len);
                    task.body_copy[rq->body_len] = '\0';
                    task.body_len = rq->body_len;
                }
            }
            snprintf(task.prompt_preview, sizeof(task.prompt_preview), "%s", prompt_preview);
            task.timestamp_us = mono_ns() / 1000ULL;

            if (!shadow_engine_submit_task(ac->shadow_eng, &task)) {
                shadow_task_free(&task);
            }
        }
    }

    tracer_span_start(
        &q->trace_ctx, "router_and_hedge", SPAN_KIND_INTERNAL, q->trace_ctx.root_span_id);

    /* --- route --- */
    if (model_router_resolve(ac->router, q->model, &q->route) != 0) {
        tracer_span_end(&q->trace_ctx, "router_and_hedge", SPAN_STATUS_ERROR, "model not found");
        aigate_write_error(rc, PIPE_MODEL, "model_not_found", "model not found");
        return -1;
    }

    /* --- candidate targets selection --- */
    q->n_candidates = 0;
    if (model_router_select_candidates_targeted(ac->cb,
                                                ac->lt,
                                                &q->route,
                                                rq->target_provider,
                                                q->candidates,
                                                MAX_TARGETS_PER_MODEL,
                                                &q->n_candidates) != 0 ||
        q->n_candidates == 0) {
        tracer_span_end(&q->trace_ctx,
                        "router_and_hedge",
                        SPAN_STATUS_ERROR,
                        "no upstream targets available for model");
        aigate_write_error(
            rc, PIPE_MODEL, "no_healthy_upstream", "no upstream targets available for model");
        return -1;
    }

    tracer_span_set_attr(&q->trace_ctx,
                         "router_and_hedge",
                         "aigate.router.selected_upstream",
                         q->candidates[0].endpoint);
    tracer_span_set_attr(&q->trace_ctx, "root", "gen_ai.request.model", q->model);
    tracer_span_end(&q->trace_ctx, "router_and_hedge", SPAN_STATUS_OK, NULL);

    /* Initialize effective body pointers (will be processed by filter_chain) */
    q->sanitized_body = NULL;
    q->sanitized_len = 0;
    q->eff_body = rq->body;
    q->eff_len = rq->body_len;
    return 0;
}

/** @brief Streaming cache accumulator's set_header shim: forces status=200 then passes to real response.
 *  @return Downstream set_header return; 0 if no downstream. */
int
stream_cache_acc_set_header(void* impl, const char* name, const char* value)
{
    stream_cache_acc_t* acc = (stream_cache_acc_t*)impl;
    if (acc != NULL && acc->orig_rc != NULL) {
        acc->orig_rc->status = 200;
        if (acc->orig_rc->set_header != NULL) {
            return acc->orig_rc->set_header(acc->orig_rc->impl, name, value);
        }
    }
    return 0;
}

/** @brief Ensure the SSE line buffer holds at least `need` bytes (doubling from 4KiB, hard cap 1MiB).
 *
 *  On cap breach or alloc failure sets overflow (passthrough-only, line discarded) and returns false.
 *  @return true if capacity now suffices. */
static bool
acc_line_reserve(stream_cache_acc_t* acc, size_t need)
{
    if (need <= acc->line_cap) {
        return true;
    }
    size_t cap = (acc->line_cap != 0) ? acc->line_cap : 4096;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > 1024 * 1024) {
        acc->overflow = true;
        return false;
    }
    char* nb = realloc(acc->line_buf, cap);
    if (nb == NULL) {
        acc->overflow = true;
        return false;
    }
    acc->line_buf = nb;
    acc->line_cap = cap;
    return true;
}

/** @brief Ensure the accumulation buffer holds `extra` more bytes (doubling from 4KiB, hard cap 512KiB).
 *
 *  On cap breach or alloc failure sets overflow (passthrough-only) and returns false.
 *  @return true if capacity now suffices. */
static bool
acc_reserve_content(stream_cache_acc_t* acc, size_t extra)
{
    if (acc->accum_len + extra + 1 <= acc->accum_cap) {
        return true;
    }
    size_t new_cap = acc->accum_cap ? acc->accum_cap * 2 : 4096;
    while (new_cap < acc->accum_len + extra + 1) {
        new_cap *= 2;
    }
    if (new_cap > 512 * 1024) {
        acc->overflow = true;
        return false;
    }
    char* nb = realloc(acc->accum_content, new_cap);
    if (nb == NULL) {
        acc->overflow = true;
        return false;
    }
    acc->accum_content = nb;
    acc->accum_cap = new_cap;
    return true;
}

/** @brief Parse one complete SSE line and fold its delta content into the stream accumulator.
 *
 *  Skips non-`data:` lines and `[DONE]`; captures id/created once and appends
 *  choices[0].delta.content (growable up to 512KiB, then sets overflow).
 *  @param acc  Stream accumulator (non-NULL).
 *  @param line Complete NUL-terminated SSE line, without trailing newline. */
static void
accumulate_sse_line(stream_cache_acc_t* acc, const char* line)
{
    if (strncmp(line, "data: ", 6) == 0 && strcmp(line, "data: [DONE]") != 0) {
        json_t* root = json_loads(line + 6, 0, NULL);
        if (root != NULL && json_is_object(root)) {
            if (acc->id[0] == '\0') {
                json_t* jid = json_object_get(root, "id");
                if (jid != NULL && json_is_string(jid)) {
                    snprintf(acc->id, sizeof(acc->id), "%s", json_string_value(jid));
                }
            }
            if (acc->created == 0) {
                json_t* jc = json_object_get(root, "created");
                if (jc != NULL && json_is_integer(jc)) {
                    acc->created = (long)json_integer_value(jc);
                }
            }
            json_t* choices = json_object_get(root, "choices");
            if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
                json_t* c0 = json_array_get(choices, 0);
                json_t* delta = json_object_get(c0, "delta");
                if (delta != NULL && json_is_object(delta)) {
                    json_t* jcnt = json_object_get(delta, "content");
                    if (jcnt != NULL && json_is_string(jcnt)) {
                        const char* ctext = json_string_value(jcnt);
                        size_t      clen = strlen(ctext);
                        if (clen > 0 && acc_reserve_content(acc, clen)) {
                            if (!acc->overflow && acc->accum_content != NULL) {
                                memcpy(acc->accum_content + acc->accum_len, ctext, clen);
                                acc->accum_len += clen;
                                acc->accum_content[acc->accum_len] = '\0';
                            }
                        }
                    }
                }
            }
            json_decref(root);
        }
    }
}

/** @brief Streaming cache accumulator's write shim: passes chunks to client while parsing SSE lines.
 *
 *  Extracts id/created and choices[0].delta.content from each `data:` JSON line
 *  and appends to accumulation buffer (max 512KiB; sets overflow on limit/alloc failure,
 *  then passthrough only). Used for cache backfill.
 *  @return Downstream write return; -1 if acc or orig_rc is NULL. */
int
stream_cache_acc_write(void* impl, const void* buf, size_t len, bool fin)
{
    stream_cache_acc_t* acc = (stream_cache_acc_t*)impl;
    if (acc == NULL || acc->orig_rc == NULL) {
        return -1;
    }
    acc->orig_rc->status = 200;
    acc->orig_rc->headers_sent = true;
    int rv = 0;
    /* Step 1: Forward chunk to downstream client (de-anonymizing tokens if active) */
    if (acc->orig_rc->write != NULL) {
        if (acc->pii_sf.map != NULL && acc->pii_sf.map->count > 0) {
            size_t needed = len + 128;
            char   stack_buf[4096];
            char*  filt_buf = (needed <= sizeof(stack_buf)) ? stack_buf : malloc(needed);
            if (filt_buf == NULL) {
                rv = acc->orig_rc->write(acc->orig_rc->impl, buf, len, fin);
            } else {
                size_t filt_len = 0;
                guardrails_stream_filter_feed(
                    &acc->pii_sf, (const char*)buf, len, filt_buf, needed, &filt_len);
                if (filt_len > 0) {
                    rv = acc->orig_rc->write(
                        acc->orig_rc->impl, filt_buf, filt_len, fin && acc->pii_sf.win_len == 0);
                }
                if (fin && acc->pii_sf.win_len > 0) {
                    char   fin_buf[64];
                    size_t fin_len = 0;
                    guardrails_stream_filter_flush(
                        &acc->pii_sf, fin_buf, sizeof(fin_buf), &fin_len);
                    if (fin_len > 0) {
                        rv = acc->orig_rc->write(acc->orig_rc->impl, fin_buf, fin_len, true);
                    }
                }
                if (filt_buf != stack_buf) {
                    free(filt_buf);
                }
            }
        } else {
            rv = acc->orig_rc->write(acc->orig_rc->impl, buf, len, fin);
        }
    }
    /* If stream overflowed or empty, bypass accumulation and continue passthrough only */
    if (len == 0 || buf == NULL || acc->overflow) {
        return rv;
    }

    const char* p = (const char*)buf;
    const char* end = p + len;
    /* Step 2: Split incoming chunks across newline delimiters (\n), handling cross-frame splits */
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (acc_line_reserve(acc, acc->line_len + seg + 1)) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';

                /* Step 3: Parse SSE data line and append delta content to full response accumulator */
                accumulate_sse_line(acc, acc->line_buf);
            }
            acc->line_len = 0;
            p = nl + 1;
        } else {
            /* Partial line trailing at the end of this TCP chunk; stash in line_buf until next chunk */
            size_t seg = (size_t)(end - p);
            if (acc_line_reserve(acc, acc->line_len + seg + 1)) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';
            } else {
                acc->line_len = 0;
            }
            p = end;
        }
    }
    return rv;
}

/** @brief Replay a cached hit as SSE: writes X-Cache:HIT/Age headers and pushes OpenAI-format chunks.
 *  @return 0 on success (also reserves key quota); non-zero on invalid cache body etc., caller falls back to upstream. */
int
cache_stream_replay(aigate_core*         ac,
                    aigate_response_ctx* rc,
                    cache_entry_t*       ce,
                    const char*          model,
                    const key_rec_t*     krec,
                    const char*          guardrail_act)
{
    rc->status = 200;
    if (rc->set_header != NULL) {
        rc->set_header(rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
        rc->set_header(rc->impl, "Cache-Control", "no-cache");
        rc->set_header(rc->impl, "Connection", "keep-alive");
        rc->set_header(rc->impl, "X-Cache", "HIT");
        rc->set_header(rc->impl, "X-Cache-Lookup-Time", "0.10ms");
        char age_str[32];
        snprintf(age_str, sizeof(age_str), "%ld", (long)(time(NULL) - ce->created_at));
        rc->set_header(rc->impl, "Age", age_str);
    }

    const char* id_str = "chatcmpl-cache";
    const char* model_str = model;
    long        created_ts = (long)ce->created_at;
    const char* content_str = "";

    json_t* root = json_loads(ce->response_body, 0, NULL);
    if (root != NULL) {
        json_t* jid = json_object_get(root, "id");
        if (jid != NULL && json_is_string(jid)) {
            id_str = json_string_value(jid);
        }
        json_t* jm = json_object_get(root, "model");
        if (jm != NULL && json_is_string(jm)) {
            model_str = json_string_value(jm);
        }
        json_t* jc = json_object_get(root, "created");
        if (jc != NULL && json_is_integer(jc)) {
            created_ts = (long)json_integer_value(jc);
        }
        json_t* choices = json_object_get(root, "choices");
        if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
            json_t* c0 = json_array_get(choices, 0);
            json_t* msg = json_object_get(c0, "message");
            if (msg != NULL) {
                json_t* cnt = json_object_get(msg, "content");
                if (cnt != NULL && json_is_string(cnt)) {
                    content_str = json_string_value(cnt);
                }
            }
        }
    }

    /* 1. Initial chunk with role */
    json_t* role_obj = json_pack("{s:s, s:s, s:I, s:s, s:[{s:i, s:{s:s, s:s}, s:n}]}",
                                 "id",
                                 id_str,
                                 "object",
                                 "chat.completion.chunk",
                                 "created",
                                 (json_int_t)created_ts,
                                 "model",
                                 model_str,
                                 "choices",
                                 "index",
                                 0,
                                 "delta",
                                 "role",
                                 "assistant",
                                 "content",
                                 "",
                                 "finish_reason");
    if (role_obj != NULL) {
        char* role_json = json_dumps(role_obj, JSON_COMPACT);
        if (role_json != NULL) {
            char line[2048];
            int  n = snprintf(line, sizeof(line), "data: %s\n\n", role_json);
            if (rc->write != NULL) {
                rc->write(rc->impl, line, n, false);
            }
            free(role_json);
        }
        json_decref(role_obj);
    }

    /* 2. Content chunks in pieces */
    size_t clen = content_str ? strlen(content_str) : 0;
    size_t pos = 0;
    while (pos < clen) {
        size_t step = clen - pos;
        if (step > 32) {
            step = 32;
        }
        char piece[33];
        memcpy(piece, content_str + pos, step);
        piece[step] = '\0';

        json_t* chunk_obj = json_pack("{s:s, s:s, s:I, s:s, s:[{s:i, s:{s:s}, s:n}]}",
                                      "id",
                                      id_str,
                                      "object",
                                      "chat.completion.chunk",
                                      "created",
                                      (json_int_t)created_ts,
                                      "model",
                                      model_str,
                                      "choices",
                                      "index",
                                      0,
                                      "delta",
                                      "content",
                                      piece,
                                      "finish_reason");
        if (chunk_obj != NULL) {
            char* chunk_json = json_dumps(chunk_obj, JSON_COMPACT);
            if (chunk_json != NULL) {
                char line[2048];
                int  n = snprintf(line, sizeof(line), "data: %s\n\n", chunk_json);
                if (rc->write != NULL) {
                    rc->write(rc->impl, line, n, false);
                }
                free(chunk_json);
            }
            json_decref(chunk_obj);
        }
        pos += step;
    }

    /* 3. Finish chunk with finish_reason and usage */
    json_t* fin_obj = json_pack("{s:s, s:s, s:I, s:s, s:[{s:i, s:{}, s:s}], s:{s:i, s:i, s:i}}",
                                "id",
                                id_str,
                                "object",
                                "chat.completion.chunk",
                                "created",
                                (json_int_t)created_ts,
                                "model",
                                model_str,
                                "choices",
                                "index",
                                0,
                                "delta",
                                "finish_reason",
                                "stop",
                                "usage",
                                "prompt_tokens",
                                (int)ce->prompt_tokens,
                                "completion_tokens",
                                (int)ce->completion_tokens,
                                "total_tokens",
                                (int)(ce->prompt_tokens + ce->completion_tokens));
    if (fin_obj != NULL) {
        char* fin_json = json_dumps(fin_obj, JSON_COMPACT);
        if (fin_json != NULL) {
            char line[2048];
            int  n = snprintf(line, sizeof(line), "data: %s\n\n", fin_json);
            if (rc->write != NULL) {
                rc->write(rc->impl, line, n, false);
            }
            free(fin_json);
        }
        json_decref(fin_obj);
    }

    /* 4. Stream terminator */
    if (rc->write != NULL) {
        rc->write(rc->impl, "data: [DONE]\n\n", 14, false);
        rc->write(rc->impl, "", 0, true);
    }

    if (root != NULL) {
        json_decref(root);
    }

    record_usage_and_event(ac,
                           krec->key_id,
                           model,
                           200,
                           ce->prompt_tokens,
                           ce->completion_tokens,
                           0,
                           0,
                           100000ULL,
                           "cache",
                           guardrail_act,
                           ce->cost_usd);
    if (ac->be != NULL) {
        budget_enforce_record(ac->be,
                              krec->key_id,
                              krec->group_id,
                              ce->cost_usd,
                              ce->prompt_tokens + ce->completion_tokens);
    }
    rl_reserve_tokens(
        ac->rl, krec->key_id, krec->daily_token_quota, ce->prompt_tokens + ce->completion_tokens);

    response_cache_release_entry(ce);
    return 0;
}

/** @brief Store a completed SSE stream into the response cache (no-op unless cacheable).
 *
 *  Packs id/model/created/content/token usage into a chat.completion JSON and sets it
 *  under q->cache_key. Pure backfill: never touches the downstream response. */
void
cache_store_stream(
    chat_req_t* q, stream_cache_acc_t* acc, long ptok, long ctok, int status, double req_cost)
{
    if (q->ac->rc != NULL && q->cache_key[0] != '\0' && !q->no_store && !acc->overflow &&
        (status == 0 || status == 200) && acc->accum_content != NULL && acc->accum_len > 0) {
        json_t* full_resp =
            json_pack("{s:s, s:s, s:I, s:s, s:[{s:i, s:{s:s, s:s}, s:s}], s:{s:i, s:i, s:i}}",
                      "id",
                      acc->id[0] ? acc->id : "chatcmpl-stream",
                      "object",
                      "chat.completion",
                      "created",
                      (json_int_t)(acc->created > 0 ? acc->created : time(NULL)),
                      "model",
                      q->model,
                      "choices",
                      "index",
                      0,
                      "message",
                      "role",
                      "assistant",
                      "content",
                      acc->accum_content,
                      "finish_reason",
                      "stop",
                      "usage",
                      "prompt_tokens",
                      (int)ptok,
                      "completion_tokens",
                      (int)ctok,
                      "total_tokens",
                      (int)(ptok + ctok));
        if (full_resp != NULL) {
            char* full_json = json_dumps(full_resp, JSON_COMPACT);
            if (full_json != NULL) {
                response_cache_set(q->ac->rc,
                                   q->cache_key,
                                   q->model,
                                   full_json,
                                   strlen(full_json),
                                   ptok,
                                   ctok,
                                   req_cost,
                                   0);
                free(full_json);
            }
            json_decref(full_resp);
        }
    }
}

/** @brief Fill a per-target route copy from base route + upstream target. */
void
fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out)
{
    *out = *route;
    snprintf(
        out->provider, sizeof out->provider, "%.*s", (int)sizeof out->provider - 1, t->provider);
    snprintf(
        out->endpoint, sizeof out->endpoint, "%.*s", (int)sizeof out->endpoint - 1, t->endpoint);
    snprintf(out->upstream_key,
             sizeof out->upstream_key,
             "%.*s",
             (int)sizeof out->upstream_key - 1,
             t->upstream_key);
}

/** @brief Post-success settlement: usage event + budget record + daily token reserve. */
void
settle_success(chat_req_t* q,
               int         status,
               long        ptok,
               long        ctok,
               long        cached_tok,
               uint64_t    lat,
               const char* provider,
               double      cost)
{
    record_usage_and_event(q->ac,
                           q->krec.key_id,
                           q->model,
                           status,
                           ptok,
                           ctok,
                           cached_tok,
                           0,
                           lat,
                           provider,
                           q->guardrail_act,
                           cost);
    if (q->ac->be != NULL) {
        budget_enforce_record(q->ac->be, q->krec.key_id, q->krec.group_id, cost, ptok + ctok);
    }
    if (ptok + ctok > 0) {
        rl_reserve_tokens(q->ac->rl, q->krec.key_id, q->krec.daily_token_quota, ptok + ctok);
    }
    aigate_record_audit(q->ac,
                        q->trace_ctx.trace_id,
                        (q->rq != NULL) ? q->rq->client_ip : NULL,
                        q->krec.key_id,
                        q->model,
                        provider,
                        status,
                        (uint32_t)ptok,
                        (uint32_t)ctok,
                        lat,
                        0,
                        (status < 400) ? AUDIT_SEV_INFO
                                       : (status == 429 ? AUDIT_SEV_WARN : AUDIT_SEV_ERROR),
                        "",
                        "",
                        NULL,
                        0);
}

/** @brief Failover warning log + failover metric. */
void
failover_warn(const char*              label,
              const char*              model,
              const upstream_target_t* from,
              const upstream_target_t* to,
              int                      status,
              int                      urc)
{
    AIGATE_LOG_WARN("%s for model %s from %s (%s) to %s (%s) due to status %d (urc %d)",
                    label,
                    model,
                    from->provider,
                    from->endpoint,
                    to->provider,
                    to->endpoint,
                    status,
                    urc);
    metrics_inc_failover(model, from->provider, to->provider);
}

int

aigate_handle_request(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc)
{
    if (rq->path != NULL && strcmp(rq->path, "/v1/responses") == 0) {
        return handle_responses(ac, rq, rc);
    }

    if (rq->path != NULL && strcmp(rq->path, "/v1/messages") == 0) {
        return handle_anthropic_messages(ac, rq, rc);
    }

    if (rq->path != NULL && (strstr(rq->path, ":generateContent") != NULL ||
                             strstr(rq->path, ":streamGenerateContent") != NULL)) {
        return handle_gemini_generate(ac, rq, rc);
    }

    chat_req_t chatq;
    memset(&chatq, 0, sizeof(chatq));
    chatq.ac = ac;
    chatq.rq = rq;
    chatq.rc = rc;

    tracer_context_init(
        &chatq.trace_ctx, rq->traceparent, ac != NULL && ac->tracer_cfg.sample_rate >= 1.0);
    tracer_span_start(&chatq.trace_ctx, "root", SPAN_KIND_SERVER, NULL);
    tracer_span_set_attr(&chatq.trace_ctx, "root", "gen_ai.system", "openai");

    if (gate_request(&chatq) != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    {
        int handled = handle_models_list(&chatq);
        if (handled != 0) {
            chat_req_cleanup(&chatq);
            return handled > 0 ? 0 : handled;
        }
    }

    if (resolve_chat_target(&chatq) != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    tracer_span_start(
        &chatq.trace_ctx, "guardrails_inbound", SPAN_KIND_INTERNAL, chatq.trace_ctx.root_span_id);
    if (filter_chain_execute_inbound(&chatq) != FILTER_CONTINUE) {
        tracer_span_set_attr_int(
            &chatq.trace_ctx, "guardrails_inbound", "aigate.guardrails.blocked", 1);
        tracer_span_end(
            &chatq.trace_ctx, "guardrails_inbound", SPAN_STATUS_ERROR, "blocked by guardrails");
        chat_req_cleanup(&chatq);
        return 0;
    }
    tracer_span_set_attr_int(&chatq.trace_ctx,
                             "guardrails_inbound",
                             "aigate.guardrails.pii_masked",
                             chatq.pii_map.count);
    tracer_span_set_attr_int(
        &chatq.trace_ctx, "guardrails_inbound", "aigate.guardrails.blocked", 0);
    tracer_span_end(&chatq.trace_ctx, "guardrails_inbound", SPAN_STATUS_OK, NULL);

    /* --- handle /v1/embeddings --- */
    if (rq->path != NULL && strcmp(rq->path, "/v1/embeddings") == 0) {
        return handle_embeddings(&chatq);
    }

    bool is_streaming = false;
    int  cache_handled = prepare_chat_cache(&chatq, &is_streaming);
    if (cache_handled != 0) {
        chat_req_cleanup(&chatq);
        return 0;
    }

    /* --- prompt compression and token pruning --- */
    if (ac != NULL && ac->comp_cache != NULL && chatq.eff_body != NULL && chatq.eff_len > 0) {
        const char* comp_hdr = rq->compress_control;
        bool        skip_comp =
            (comp_hdr != NULL && (strcmp(comp_hdr, "off") == 0 || strcmp(comp_hdr, "none") == 0));
        if (!skip_comp) {
            uint32_t est_tokens =
                compressor_estimate_tokens((const char*)chatq.eff_body, chatq.eff_len);
            compressor_rule_t matched_rule;
            bool              have_rule = false;

            pthread_mutex_lock(&ac->comp_rules_lock);
            for (int i = 0; i < ac->n_comp_rules; i++) {
                if (compressor_rule_match(&ac->comp_rules[i], chatq.model, est_tokens)) {
                    matched_rule = ac->comp_rules[i];
                    have_rule = true;
                    break;
                }
            }
            pthread_mutex_unlock(&ac->comp_rules_lock);

            if (!have_rule && comp_hdr != NULL) {
                if (strcmp(comp_hdr, "moderate") == 0 || strcmp(comp_hdr, "aggressive") == 0) {
                    memset(&matched_rule, 0, sizeof(matched_rule));
                    matched_rule.enabled = true;
                    matched_rule.level = (strcmp(comp_hdr, "aggressive") == 0)
                                             ? COMPRESS_LEVEL_AGGRESSIVE
                                             : COMPRESS_LEVEL_MODERATE;
                    matched_rule.min_tokens = 0;
                    matched_rule.max_history_turns = 6;
                    matched_rule.target_ratio = 0.60;
                    matched_rule.preserve_system = true;
                    matched_rule.preserve_code = true;
                    matched_rule.preserve_tools = true;
                    have_rule = true;
                }
            } else if (have_rule && comp_hdr != NULL) {
                if (strcmp(comp_hdr, "aggressive") == 0) {
                    matched_rule.level = COMPRESS_LEVEL_AGGRESSIVE;
                } else if (strcmp(comp_hdr, "moderate") == 0) {
                    matched_rule.level = COMPRESS_LEVEL_MODERATE;
                }
            }

            if (have_rule) {
                tracer_span_start(&chatq.trace_ctx,
                                  "prompt_compression",
                                  SPAN_KIND_INTERNAL,
                                  chatq.trace_ctx.root_span_id);
                bool ok = prompt_compressor_process_payload(
                    (const char*)chatq.eff_body, chatq.eff_len, &matched_rule, &chatq.comp_result);
                if (ok && chatq.comp_result.compressed &&
                    chatq.comp_result.compressed_payload != NULL) {
                    chatq.eff_body = chatq.comp_result.compressed_payload;
                    chatq.eff_len = chatq.comp_result.compressed_len;

                    tracer_span_set_attr(&chatq.trace_ctx,
                                         "prompt_compression",
                                         "aigate.compression.applied",
                                         "true");
                    tracer_span_set_attr(&chatq.trace_ctx,
                                         "prompt_compression",
                                         "aigate.compression.level",
                                         matched_rule.level == COMPRESS_LEVEL_AGGRESSIVE
                                             ? "aggressive"
                                             : "moderate");
                    tracer_span_set_attr_int(&chatq.trace_ctx,
                                             "prompt_compression",
                                             "aigate.compression.original_tokens",
                                             chatq.comp_result.original_tokens);
                    tracer_span_set_attr_int(&chatq.trace_ctx,
                                             "prompt_compression",
                                             "aigate.compression.compressed_tokens",
                                             chatq.comp_result.compressed_tokens);
                    tracer_span_set_attr_int(&chatq.trace_ctx,
                                             "prompt_compression",
                                             "aigate.compression.saved_tokens",
                                             chatq.comp_result.saved_tokens);
                    char ratio_buf[32];
                    snprintf(
                        ratio_buf, sizeof(ratio_buf), "%.2f", chatq.comp_result.compression_ratio);
                    tracer_span_set_attr(&chatq.trace_ctx,
                                         "prompt_compression",
                                         "aigate.compression.ratio",
                                         ratio_buf);
                    tracer_span_set_attr_int(&chatq.trace_ctx,
                                             "prompt_compression",
                                             "aigate.compression.duration_us",
                                             chatq.comp_result.elapsed_us);
                    tracer_span_end(&chatq.trace_ctx, "prompt_compression", SPAN_STATUS_OK, NULL);

                    if (rc->set_header != NULL) {
                        char hbuf[32];
                        snprintf(hbuf, sizeof(hbuf), "%u", chatq.comp_result.original_tokens);
                        rc->set_header(rc->impl, "X-Aigate-Prompt-Original-Tokens", hbuf);
                        snprintf(hbuf, sizeof(hbuf), "%u", chatq.comp_result.compressed_tokens);
                        rc->set_header(rc->impl, "X-Aigate-Prompt-Compressed-Tokens", hbuf);
                        snprintf(hbuf, sizeof(hbuf), "%.2f", chatq.comp_result.compression_ratio);
                        rc->set_header(rc->impl, "X-Aigate-Compression-Ratio", hbuf);
                        snprintf(hbuf, sizeof(hbuf), "%u", chatq.comp_result.saved_tokens);
                        rc->set_header(rc->impl, "X-Aigate-Compression-Saved-Tokens", hbuf);
                    }

                    compressor_snapshot_t snap;
                    memset(&snap, 0, sizeof(snap));
                    snprintf(snap.req_id, sizeof(snap.req_id), "%s", chatq.trace_ctx.trace_id);
                    snprintf(snap.model, sizeof(snap.model), "%s", chatq.model);
                    snap.timestamp = (int64_t)time(NULL);
                    snap.original_tokens = chatq.comp_result.original_tokens;
                    snap.compressed_tokens = chatq.comp_result.compressed_tokens;
                    snap.saved_tokens = chatq.comp_result.saved_tokens;
                    snap.compression_ratio = chatq.comp_result.compression_ratio;
                    snap.elapsed_us = chatq.comp_result.elapsed_us;
                    extract_prompt_snippet(
                        chatq.jbody, snap.prompt_preview, sizeof(snap.prompt_preview));
                    if (chatq.rq->body != NULL) {
                        snprintf(snap.orig_preview,
                                 sizeof(snap.orig_preview),
                                 "%.511s",
                                 (const char*)chatq.rq->body);
                    }
                    if (chatq.comp_result.compressed_payload != NULL) {
                        snprintf(snap.comp_preview,
                                 sizeof(snap.comp_preview),
                                 "%.511s",
                                 chatq.comp_result.compressed_payload);
                    }
                    compressor_cache_record(ac->comp_cache, &snap);
                } else {
                    tracer_span_set_attr(&chatq.trace_ctx,
                                         "prompt_compression",
                                         "aigate.compression.applied",
                                         "false");
                    tracer_span_end(&chatq.trace_ctx, "prompt_compression", SPAN_STATUS_OK, NULL);
                }
            }
        }
    }

    /* --- prompt cache optimizer: prefix alignment, tool sorting, timestamp sinking & breakpoints --- */
    const char* prompt_cache_hdr = rq->prompt_cache_control;
    bool        prompt_cache_disabled =
        (prompt_cache_hdr != NULL &&
         (strcmp(prompt_cache_hdr, "off") == 0 || strcmp(prompt_cache_hdr, "false") == 0 ||
          strcmp(prompt_cache_hdr, "no") == 0));

    if (!prompt_cache_disabled && ac != NULL) {
        cache_optimizer_rule_t opt_rule;
        bool                   have_opt_rule = false;

        pthread_mutex_lock(&ac->cache_opt_rules_lock);
        for (int i = 0; i < ac->n_cache_opt_rules; i++) {
            if (ac->cache_opt_rules[i].enabled &&
                cache_optimizer_rule_matches(&ac->cache_opt_rules[i], chatq.model)) {
                opt_rule = ac->cache_opt_rules[i];
                have_opt_rule = true;
                break;
            }
        }
        pthread_mutex_unlock(&ac->cache_opt_rules_lock);

        if (!have_opt_rule && prompt_cache_hdr != NULL) {
            if (strcmp(prompt_cache_hdr, "on") == 0 || strcmp(prompt_cache_hdr, "true") == 0 ||
                strcmp(prompt_cache_hdr, "auto") == 0 || strcmp(prompt_cache_hdr, "force") == 0) {
                memset(&opt_rule, 0, sizeof(opt_rule));
                opt_rule.enabled = true;
                opt_rule.sort_tools = true;
                opt_rule.sink_dynamic_system = true;
                opt_rule.inject_anthropic_breakpoints = true;
                opt_rule.min_tokens_threshold = 1024;
                have_opt_rule = true;
            }
        }

        if (have_opt_rule) {
            tracer_span_start(&chatq.trace_ctx,
                              "prompt_cache_optimizer",
                              SPAN_KIND_INTERNAL,
                              chatq.trace_ctx.root_span_id);
            bool ok = cache_optimizer_process_payload(
                (const char*)chatq.eff_body, chatq.eff_len, &opt_rule, &chatq.cache_opt_result);
            if (ok && chatq.cache_opt_result.optimized &&
                chatq.cache_opt_result.optimized_payload != NULL) {
                chatq.eff_body = chatq.cache_opt_result.optimized_payload;
                chatq.eff_len = chatq.cache_opt_result.optimized_len;

                tracer_span_set_attr(&chatq.trace_ctx,
                                     "prompt_cache_optimizer",
                                     "aigate.prompt_cache.optimized",
                                     "true");
                tracer_span_set_attr(&chatq.trace_ctx,
                                     "prompt_cache_optimizer",
                                     "aigate.prompt_cache.tools_sorted",
                                     chatq.cache_opt_result.tools_sorted ? "true" : "false");
                tracer_span_set_attr(&chatq.trace_ctx,
                                     "prompt_cache_optimizer",
                                     "aigate.prompt_cache.dynamic_sunk",
                                     chatq.cache_opt_result.dynamic_sunk ? "true" : "false");
                tracer_span_set_attr_int(&chatq.trace_ctx,
                                         "prompt_cache_optimizer",
                                         "aigate.prompt_cache.breakpoints_injected",
                                         chatq.cache_opt_result.breakpoints_injected);
                tracer_span_set_attr_int(&chatq.trace_ctx,
                                         "prompt_cache_optimizer",
                                         "aigate.prompt_cache.duration_us",
                                         chatq.cache_opt_result.latency_us);
                tracer_span_end(&chatq.trace_ctx, "prompt_cache_optimizer", SPAN_STATUS_OK, NULL);
            } else {
                tracer_span_set_attr(&chatq.trace_ctx,
                                     "prompt_cache_optimizer",
                                     "aigate.prompt_cache.optimized",
                                     "false");
                tracer_span_end(&chatq.trace_ctx, "prompt_cache_optimizer", SPAN_STATUS_OK, NULL);
            }
        }
    }

    if (is_streaming) {
        return handle_chat_stream(&chatq);
    }

    /* --- upstream non-streaming call with failover loop --- */
    return handle_chat_sync(&chatq);
}