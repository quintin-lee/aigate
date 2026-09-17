/** @file lru.h
 *  @brief Mutex-protected LRU map with caller-owned opaque values.
 *
 *  Values are opaque pointers owned by the caller; the evict callback
 *  (when non-NULL) receives each value being displaced so the owner can
 *  release it. A NULL entry removal via lru_invalidate() does NOT invoke
 *  the evict callback.
 */
#ifndef AIGATE_LRU_H
#define AIGATE_LRU_H

#include <stddef.h>

/** @brief Eviction callback; @p val is a value displaced by capacity or free. */
typedef void (*lru_evict_fn)(void *val);

typedef struct lru lru_t;

/** @brief Create an LRU holding at most @p capacity entries.
 * @param capacity  max live entries; must be >= 1
 * @param on_evict  called with displaced values (may be NULL)
 * @return new instance, or NULL on allocation failure. */
lru_t *lru_new(size_t capacity, lru_evict_fn on_evict);

/** @brief Free the map; invokes on_evict for every remaining value. */
void lru_free(lru_t *lr);

/** @brief Lookup; refreshes recency on hit. @return stored value or NULL on miss. */
void *lru_get(lru_t *lr, const char *key);

/** @brief Insert/replace a value and refresh recency; evicts LRU entry when full. */
void lru_put(lru_t *lr, const char *key, void *val);

/** @brief Remove an entry without invoking on_evict. @return 1 if present, 0 otherwise. */
int lru_invalidate(lru_t *lr, const char *key);

/** @brief Number of live entries. */
size_t lru_size(const lru_t *lr);

#endif /* AIGATE_LRU_H */
