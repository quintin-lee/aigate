/** @file lru.h
 *  @brief Mutex-protected LRU map with caller-owned opaque values.
 *
 *  Values are opaque pointers owned by the LRU: the evict callback
 *  (when non-NULL) receives every displaced value — on capacity
 *  eviction, on lru_invalidate removal, and on lru_free — so the
 *  owner can release it.
 */
#ifndef AIGATE_LRU_H
#define AIGATE_LRU_H

#include <stddef.h>

/** @brief Eviction callback; @p val is a value displaced by capacity or free. */
typedef void (*lru_evict_fn)(void* val);

typedef struct lru lru_t;

/** @brief Create an LRU holding at most @p capacity entries.
 * @param capacity  max live entries; must be >= 1
 * @param on_evict  called with displaced values (may be NULL)
 * @return new instance, or NULL on allocation failure. */
lru_t* lru_new(size_t capacity, lru_evict_fn on_evict);

/** @brief Free the map; invokes on_evict for every remaining value. */
void lru_free(lru_t* lr);

/** @brief Lookup; refreshes recency on hit. @return stored value or NULL on miss. */
void* lru_get(lru_t* lr, const char* key);

/** @brief Insert/replace a value and refresh recency.
 * @note Replacement of an existing key and capacity eviction both hand
 *       the displaced value to on_evict; the LRU owns all values. */
void lru_put(lru_t* lr, const char* key, void* val);

/** @brief Remove an entry, invoking on_evict for its value. @return 1 if present, 0 otherwise. */
int lru_invalidate(lru_t* lr, const char* key);

/** @brief Number of live entries. */
size_t lru_size(const lru_t* lr);

#endif /* AIGATE_LRU_H */
