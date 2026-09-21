/** @file auth_key.h
 *  @brief Client key verification: Bearer → SHA-256 → LRU/PG, constant-time.
 *
 *  Resolves the calling key on the hot path. Records are cached in an LRU
 *  keyed by the SHA-256 hex of the Bearer token; misses fall through to the
 *  PG ops. Admin mutations invalidate the cached entry so the next request
 *  re-reads the store.
 */
#ifndef AIGATE_AUTH_KEY_H
#define AIGATE_AUTH_KEY_H

#include "lru.h"
#include "pg_store.h"

/** @brief Auth cache state (owned by the core; one instance per process). */
typedef struct auth_key_cache {
    lru_t*   recs;    /* key_hash(hex) → key_rec_t* (heap values, evict frees) */
    lru_t*   neg;     /* key_hash(hex) → sentinel: unknown keys (no PG re-query) */
    pg_ops_t ops;     /* backing ops table (borrowed, not owned) */
    void*    ops_ctx; /* ops->ctx */
} auth_key_cache;

/** @brief Initialize the cache. @param ps the backing store; @return 0 ok, -1 OOM.
 * @note Copies the ops table + ctx from the store; the caller keeps owning
 *       the store. */
int auth_key_init(auth_key_cache* akc, pg_store_t* ps);

/** @brief Tear down; frees the LRU and any cached key records. */
void auth_key_shutdown(auth_key_cache* akc);

/** @brief Resolve a Bearer token to a key record.
 * @return 0 + @p out filled; @p out is a deep copy owned by the caller
 *         (release with key_rec_free).
 * @return -1 unknown key or allocation failure; -2 revoked; -3 expired.
 * @invariant out is zeroed on every return; on -1/-2/-3 the caller may
 *            still key_rec_free(out) safely. */
int auth_key_resolve(auth_key_cache* akc, const char* bearer, key_rec_t* out);

/** @brief Invalidate the cached entry for @p key_hash (call after admin changes). */
void auth_key_invalidate(auth_key_cache* akc, const char* key_hash);

/** @brief 1 if @p k permits @p model (empty allowlist = all models). */
int key_allows_model(const key_rec_t* k, const char* model);

#endif /* AIGATE_AUTH_KEY_H */
