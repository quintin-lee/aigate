/** @file aigate_core_internal.h
 *  @brief Internal types and shared helpers across pipeline modules in src/core/.
 *  This header is internal to src/core/ and must not be included by external consumers.
 */
#ifndef AIGATE_CORE_INTERNAL_H
#define AIGATE_CORE_INTERNAL_H

#include "aigate_core.h"
#include "aigate_log.h"
#include "guardrails.h"
#include "response_cache.h"
#include "observe/tracer.h"
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Pipeline exit and error HTTP status codes returned by internal stages. */
enum {
    PIPE_OK = 0,            /**< Request completed successfully or handled. */
    PIPE_AUTH = 401,        /**< Authentication failed or invalid API key. */
    PIPE_FORBIDDEN = 403,   /**< Forbidden by policy or model access controls. */
    PIPE_RATE = 429,        /**< Rate limit or budget quota exceeded. */
    PIPE_MODEL = 404,       /**< Requested model not found or unavailable. */
    PIPE_UPSTREAM = 502,    /**< Upstream provider communication failure. */
    PIPE_UNSUPPORTED = 501, /**< Feature or endpoint not supported by provider. */
};

/** @brief Streaming cache accumulator for real-time SSE chunk passthrough and full content aggregation. */
typedef struct stream_cache_acc {
    aigate_response_ctx* orig_rc;       /**< Real response context (borrowed). */
    char*                accum_content; /**< Accumulated full body (max 512KiB). */
    size_t               accum_len;     /**< Accumulated byte count. */
    size_t               accum_cap;     /**< accum_content capacity. */
    char*                line_buf;      /**< Growable SSE line buffer. */
    size_t               line_cap;      /**< line_buf capacity. */
    size_t               line_len;      /**< Valid line buffer length. */
    char                 id[64];        /**< Response id (from first data line). */
    long                 created;       /**< Response created timestamp. */
    bool                 overflow;      /**< Over-limit/alloc failure: passthrough only. */
    pii_stream_filter_t  pii_sf; /**< Streaming PII de-anonymization sliding window filter. */
} stream_cache_acc_t;

/**
 * @brief Unified request processing context spanning routing, admission, and execution.
 *
 * Encapsulates the lifecycle of a request as it passes through admission control,
 * guardrails inspection, upstream selection, cache checks, and dispatch.
 */
typedef struct chat_req {
    aigate_core*         ac;    /**< Reference to core gateway engine instance. */
    aigate_request_ctx*  rq;    /**< Inbound client request context (headers, path, method). */
    aigate_response_ctx* rc;    /**< Outbound client response sink. */
    key_rec_t            krec;  /**< Authenticated API key record and quota policy. */
    json_t*              jbody; /**< Parsed JSON request body AST (caller/lifecycle owned). */
    const char*          model; /**< Target model identifier requested by client. */
    model_rec_t          route; /**< Primary model route configuration from store/cache. */
    upstream_target_t
        candidates[MAX_TARGETS_PER_MODEL]; /**< Candidate upstreams ordered by priority/latency. */
    int n_candidates;      /**< Number of active candidates populated in candidates. */
    char*  sanitized_body; /**< Allocated sanitized payload after guardrail/template transforms. */
    size_t sanitized_len;  /**< Byte length of sanitized_body. */
    const void* eff_body;  /**< Effective body pointer sent upstream (raw or sanitized). */
    size_t      eff_len;   /**< Byte length of eff_body. */
    char        guardrail_act[16]; /**< Guardrails decision tag (e.g. "pass", "mask", "block"). */
    char        cache_key[65]; /**< Hex-encoded SHA-256 fingerprint for response cache lookup. */
    bool        bypass_cache;  /**< True if cache lookup should be bypassed by policy or header. */
    bool        no_store;      /**< True if response should not be cached. */
    pii_session_map_t pii_map; /**< Request-bound PII de-anonymization session map. */
    trace_context_t   trace_ctx;        /**< OpenTelemetry trace context and recorded spans. */
    bool              is_canary;        /**< True if canary routing was applied. */
    char              canary_model[64]; /**< Storage buffer for rewritten canary model name. */
    long              canary_rule_id;   /**< Rule ID of active canary route. */
    bool              has_shadow;       /**< True if request is being shadowed asynchronously. */
    shadow_rule_t     shadow_rule;      /**< Active shadow rule snapshot. */
    char              eval_id[33];      /**< Unique evaluation ID for shadow/canary pairing. */
} chat_req_t;

/**
 * @brief Get current monotonic clock time in nanoseconds.
 * @return Monotonic time in nanoseconds.
 */
uint64_t mono_ns(void);

/**
 * @brief Calculate estimated request cost in USD based on model pricing and token counts.
 */
double calc_req_cost(const model_rec_t* route, long prompt, long completion, long cached);

/**
 * @brief Record request metrics into usage meter and emit event bus notification.
 */
void record_usage_and_event(aigate_core* ac,
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
                            double       req_cost);

/**
 * @brief Fill current effective model route by overlaying candidate target parameters.
 * @param[in]  route Base model route definition.
 * @param[in]  t     Active upstream target candidate.
 * @param[out] out   Target route configuration to populate.
 */
void fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out);

/**
 * @brief Settle a successful request completion, updating usage meters and logs.
 * @param[in,out] q           Request processing context.
 * @param[in]     status      Upstream HTTP status code.
 * @param[in]     ptok        Prompt tokens consumed.
 * @param[in]     ctok        Completion tokens produced.
 * @param[in]     cached_tok  Cached tokens reused.
 * @param[in]     lat         Request latency in nanoseconds.
 * @param[in]     provider    Upstream provider identifier.
 * @param[in]     cost        Estimated cost in USD.
 */
void settle_success(chat_req_t* q,
                    int         status,
                    long        ptok,
                    long        ctok,
                    long        cached_tok,
                    uint64_t    lat,
                    const char* provider,
                    double      cost);

/**
 * @brief Emit a warning log for an upstream failover event.
 * @param[in] label  Failover context label.
 * @param[in] model  Target model name.
 * @param[in] from   Failing upstream target.
 * @param[in] to     Next fallback upstream target.
 * @param[in] status HTTP status code from failed target.
 * @param[in] urc    Upstream transport error return code.
 */
void failover_warn(const char*              label,
                   const char*              model,
                   const upstream_target_t* from,
                   const upstream_target_t* to,
                   int                      status,
                   int                      urc);

/**
 * @brief Perform gateway admission checks (auth, ratelimit, budgets, guardrails).
 * @param[in,out] q Request processing context.
 * @return 0 on admitted; non-zero HTTP error code on rejected.
 */
int gate_request(chat_req_t* q);

/**
 * @brief Resolve model route and prioritize upstream candidate targets.
 * @param[in,out] q Request processing context.
 * @return 0 on success; non-zero HTTP error code on failure.
 */
int resolve_chat_target(chat_req_t* q);

/**
 * @brief Clean up allocated request resources in chat_req_t.
 * @param[in,out] q Request processing context to finalize.
 */
void chat_req_cleanup(chat_req_t* q);

/**
 * @brief Set HTTP header in stream cache accumulator.
 * @param[in,out] impl  Pointer to stream_cache_acc_t.
 * @param[in]     name  Header name string.
 * @param[in]     value Header value string.
 * @return 0 on success, non-zero on failure.
 */
int stream_cache_acc_set_header(void* impl, const char* name, const char* value);

/**
 * @brief Write chunk data into stream cache accumulator and flush to downstream client.
 * @param[in,out] impl Pointer to stream_cache_acc_t.
 * @param[in]     buf  Chunk buffer.
 * @param[in]     len  Length of chunk buffer in bytes.
 * @param[in]     fin  True if this is the final chunk.
 * @return 0 on success, non-zero on error.
 */
int stream_cache_acc_write(void* impl, const void* buf, size_t len, bool fin);

/**
 * @brief Replay cached response stream to client.
 * @param[in]     ac            Core engine context.
 * @param[in,out] rc            Response context.
 * @param[in]     ce            Cache entry to replay.
 * @param[in]     model         Model identifier.
 * @param[in]     krec          Key record.
 * @param[in]     guardrail_act Guardrail action string.
 * @return 0 on success, non-zero on failure.
 */
int cache_stream_replay(aigate_core*         ac,
                        aigate_response_ctx* rc,
                        cache_entry_t*       ce,
                        const char*          model,
                        const key_rec_t*     krec,
                        const char*          guardrail_act);

/**
 * @brief Store accumulated streaming response into response cache.
 * @param[in,out] q        Request context.
 * @param[in]     acc      Accumulator containing complete stream content.
 * @param[in]     ptok     Prompt tokens.
 * @param[in]     ctok     Completion tokens.
 * @param[in]     status   HTTP status code.
 * @param[in]     req_cost Cost in USD.
 */
void cache_store_stream(
    chat_req_t* q, stream_cache_acc_t* acc, long ptok, long ctok, int status, double req_cost);

/**
 * @brief Handle /v1/embeddings endpoint execution.
 */
int handle_embeddings(chat_req_t* q);

/**
 * @brief Handle native Anthropic /v1/messages endpoint request.
 * @param[in]     ac Core engine context.
 * @param[in]     rq Inbound request context.
 * @param[in,out] rc Outbound response context.
 * @return 0 on success, non-zero on error.
 */
int handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/**
 * @brief Handle native Gemini generateContent endpoint request.
 * @param[in]     ac Core engine context.
 * @param[in]     rq Inbound request context.
 * @param[in,out] rc Outbound response context.
 * @return 0 on success, non-zero on error.
 */
int handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/**
 * @brief Handle OpenAI /v1/responses endpoint request.
 * @param[in]     ac Core engine context.
 * @param[in]     rq Inbound request context.
 * @param[in,out] rc Outbound response context.
 * @return 0 on success, non-zero on error.
 */
int handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);

/**
 * @brief Handle /v1/models endpoint query.
 * @param[in,out] q Request context.
 * @return 0 on success, non-zero on error.
 */
int handle_models_list(chat_req_t* q);

/**
 * @brief Prepare chat cache lookup and check capabilities.
 */
int prepare_chat_cache(chat_req_t* q, bool* is_streaming);

/**
 * @brief Handle synchronous (non-streaming) /v1/chat/completions request.
 * @param[in,out] q Request context.
 * @return 0 on success, non-zero on error.
 */
int handle_chat_sync(chat_req_t* q);

/**
 * @brief Handle streaming (SSE) /v1/chat/completions request.
 */
int handle_chat_stream(chat_req_t* q);

#endif /* AIGATE_CORE_INTERNAL_H */
