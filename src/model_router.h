/** @file model_router.h
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
    lru_t*   routes;      /* model_name → model_rec_t* (heap values, evict frees) */
    pg_ops_t ops;         /* borrowed ops table */
    void*    ops_ctx;
    uint8_t  master[32];
    int      have_master; /* 1 when AIGATE_MASTER_KEY was provided */
} model_router_t;

/** @brief New router over @p ps; @p master (32 bytes) enables pg: secrets.
 * @return router, or NULL on OOM. */
model_router_t* model_router_new(pg_store_t* ps, const uint8_t* master);

/** @brief Free the router + all cached routes. */
void model_router_free(model_router_t* mr);

/** @brief Resolve a model name into a fully populated route.
 * @param out   caller-provided record (e.g. on the stack); filled in place,
 *              no allocation, nothing to release.
 * @return 0 + @p out filled (including upstream_key); -1 unknown/disabled
 *         model or allocation failure; -2 upstream key unresolvable.
 * @note out->upstream_key is filled from an env lookup or pg decrypt. */
int model_router_resolve(model_router_t* mr, const char* model, model_rec_t* out);

/** @brief Invalidate the cached route after an admin model mutation. */
void model_router_invalidate(model_router_t* mr, const char* model);

#endif /* AIGATE_MODEL_ROUTER_H */
