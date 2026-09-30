/** @file aigate_core_internal.h
 *  @brief Internal types and shared helpers across pipeline modules in src/core/.
 *  This header is internal to src/core/ and must not be included by external consumers.
 */
#ifndef AIGATE_CORE_INTERNAL_H
#define AIGATE_CORE_INTERNAL_H

#include "aigate_core.h"
#include "aigate_log.h"
#include "response_cache.h"
#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pipeline exit and error status codes */
enum {
    PIPE_OK = 0,
    PIPE_AUTH = 401,
    PIPE_FORBIDDEN = 403,
    PIPE_RATE = 429,
    PIPE_MODEL = 404,
    PIPE_UPSTREAM = 502,
    PIPE_UNSUPPORTED = 501,
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
} stream_cache_acc_t;

/** @brief Request processing context containing route, credentials, and payload state. */
typedef struct {
    aigate_core*         ac;
    aigate_request_ctx*  rq;
    aigate_response_ctx* rc;
    key_rec_t            krec;
    json_t*              jbody;
    const char*          model;
    model_rec_t          route;
    upstream_target_t    candidates[MAX_TARGETS_PER_MODEL];
    int                  n_candidates;
    char*                sanitized_body;
    size_t               sanitized_len;
    const void*          eff_body;
    size_t               eff_len;
    char                 guardrail_act[16];
    char                 cache_key[65];
    bool                 bypass_cache;
    bool                 no_store;
} chat_req_t;

/* Monotonic time and usage utilities */
uint64_t mono_ns(void);
double   calc_req_cost(const model_rec_t* route, long prompt, long completion, long cached);
void     record_usage_and_event(aigate_core* ac,
                                long         key_id,
                                const char*  model,
                                int          status,
                                long         prompt_tokens,
                                long         completion_tokens,
                                long         cached_tokens,
                                long         reasoning_tokens,
                                uint64_t     lat_ns,
                                const char*  provider,
                                const char*  guardrail_action,
                                double       cost_usd);

/* Route & failover settlement helpers */
void fill_cur_route(const model_rec_t* route, const upstream_target_t* t, model_rec_t* out);
void settle_success(chat_req_t* q,
                    int         status,
                    long        ptok,
                    long        ctok,
                    long        cached_tok,
                    uint64_t    lat,
                    const char* provider,
                    double      cost);
void failover_warn(const char*              label,
                   const char*              model,
                   const upstream_target_t* from,
                   const upstream_target_t* to,
                   int                      status,
                   int                      urc);

/* Gateway admission & target resolution */
int  gate_request(chat_req_t* q);
int  resolve_chat_target(chat_req_t* q);
void chat_req_cleanup(chat_req_t* q);

/* Stream cache engine operations */
int  stream_cache_acc_set_header(void* impl, const char* name, const char* value);
int  stream_cache_acc_write(void* impl, const void* buf, size_t len, bool fin);
int  cache_stream_replay(aigate_core*         ac,
                         aigate_response_ctx* rc,
                         cache_entry_t*       ce,
                         const char*          model,
                         const key_rec_t*     krec,
                         const char*          guardrail_act);
void cache_store_stream(
    chat_req_t* q, stream_cache_acc_t* acc, long ptok, long ctok, int status, double req_cost);

/* Pipeline prototypes */
int handle_embeddings(chat_req_t* q);
int handle_anthropic_messages(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_gemini_generate(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_responses(aigate_core* ac, aigate_request_ctx* rq, aigate_response_ctx* rc);
int handle_models_list(chat_req_t* q);
int prepare_chat_cache(chat_req_t* q, bool* is_streaming);
int handle_chat_sync(chat_req_t* q);
int handle_chat_stream(chat_req_t* q);

#endif /* AIGATE_CORE_INTERNAL_H */
