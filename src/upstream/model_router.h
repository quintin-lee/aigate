/** @file model_router.h
 *  @ingroup group_upstream
 *  @brief model name → route (provider/endpoint/key/params) with LRU cache.
 *
 *  Resolving a route pulls the model record from the PG ops, then resolves
 *  its upstream_key_ref (env: or pg: blob) into route->upstream_key.
 *  Routes are cached in an LRU keyed by model name; admin mutations
 *  invalidate the entry so the next request re-reads the store.
 */
#ifndef AIGATE_MODEL_ROUTER_H
#define AIGATE_MODEL_ROUTER_H

#include <stdint.h>
#include "lru.h"
#include "pg_store.h"

/** @brief Model router state (owned by the core; one per process). */
typedef struct model_router {
    lru_t*   routes;      /**< model_name → model_rec_t* (heap values, freed on eviction) */
    pg_ops_t ops;         /**< Borrowed ops table. */
    void*    ops_ctx;     /**< Ops context (borrowed). */
    uint8_t  master[32];  /**< Master key (32 bytes, for decrypting pg: refs). */
    int      have_master; /**< 1 when AIGATE_MASTER_KEY was provided */
} model_router_t;

/** @brief New router over @p ps; @p master (32 bytes) enables pg: secrets.
 * @return router, or NULL on OOM. */
model_router_t* model_router_new(pg_store_t* ps, const uint8_t* master);

/** @brief Free the router + all cached routes. */
void model_router_free(model_router_t* mr);

/** @brief Resolve a model name into a fully populated route.
 * @param mr    router (borrowed)
 * @param model model name to resolve
 * @param out   caller-provided record (e.g. on the stack); filled in place,
 *              no allocation, nothing to release.
 * @return 0 + @p out filled (including upstream_key); -1 unknown/disabled
 *         model or allocation failure; -2 upstream key unresolvable.
 * @note out->upstream_key is filled from an env lookup or pg decrypt. */
int model_router_resolve(model_router_t* mr, const char* model, model_rec_t* out);

#include "circuit_breaker.h"

/** @brief Invalidate the cached route after an admin model mutation. */
void model_router_invalidate(model_router_t* mr, const char* model);

struct latency_tracker;
typedef struct latency_tracker latency_tracker_t;

/** @brief Select ordered candidate targets for a request based on priority,
 *         load balancing policy, circuit breaker status, and adaptive latency metrics.
 * @param cb             Optional circuit breaker to check target health (can be NULL).
 * @param lt             Optional latency tracker for adaptive routing (can be NULL).
 * @param model          Model record containing targets and lb_policy.
 * @param out_candidates Array of size @p cap to receive ordered candidates.
 * @param cap            Maximum number of candidates (e.g. MAX_TARGETS_PER_MODEL).
 * @param out_count      Receives number of candidates placed in @p out_candidates.
 * @return 0 on success, -1 on error (e.g. invalid arguments or 0 targets).
 */
int model_router_select_candidates(circuit_breaker_t* cb,
                                   latency_tracker_t* lt,
                                   const model_rec_t* model,
                                   upstream_target_t* out_candidates,
                                   int                cap,
                                   int*               out_count);

/** @brief Select ordered candidate targets, optionally pinning a target provider to first position.
 *  @param cb              Optional circuit breaker to check target health (can be NULL).
 *  @param lt              Optional latency tracker for adaptive routing (can be NULL).
 *  @param model           Model record containing targets and lb_policy.
 *  @param target_provider If non-NULL and matches a configured target, pins it as first candidate.
 *  @param out_candidates  Array of size @p cap to receive ordered candidates.
 *  @param cap             Maximum number of candidates (e.g. MAX_TARGETS_PER_MODEL).
 *  @param out_count       Receives number of candidates placed in @p out_candidates.
 *  @return 0 on success, -1 on error.
 */
int model_router_select_candidates_targeted(circuit_breaker_t* cb,
                                            latency_tracker_t* lt,
                                            const model_rec_t* model,
                                            const char*        target_provider,
                                            upstream_target_t* out_candidates,
                                            int                cap,
                                            int*               out_count);

/** @brief Apply canary routing rules to a request.
 *  @param mr                  Model router (borrowed).
 *  @param cb                  Circuit breaker to verify candidate health (can be NULL).
 *  @param rules               Array of shadow/canary rules.
 *  @param num_rules           Number of rules in @p rules.
 *  @param source_model        Source model requested by client.
 *  @param header_str          Header string to match against rules.
 *  @param out_effective_model Buffer to receive effective model name.
 *  @param out_model_sz        Capacity of out_effective_model buffer.
 *  @param out_is_canary       Receives true if canary routing was applied.
 *  @param out_canary_rule_id  Receives rule ID if canary routing was applied.
 *  @return 0 on success, negative on error.
 */
int model_router_apply_canary(model_router_t*      mr,
                              circuit_breaker_t*   cb,
                              const shadow_rule_t* rules,
                              int                  num_rules,
                              const char*          source_model,
                              const char*          header_str,
                              char*                out_effective_model,
                              size_t               out_model_sz,
                              bool*                out_is_canary,
                              long*                out_canary_rule_id);

#endif /* AIGATE_MODEL_ROUTER_H */
