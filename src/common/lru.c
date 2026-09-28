/** @file lru.c
 *  @brief LRU map: doubly-linked recency list + separate-chaining hash table.
 *
 *  Each node participates in two independent chains:
 *    - recency list via n->prev / n->next   (most-recent ... least-recent)
 *    - bucket chain  via n->next_bkt         (hash table)
 *  @invariant recency list length == live entry count == live table entries.
 *  All operations take the mutex.
 */
#include "lru.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** @brief Hash-table node; links recency list and bucket chain. */
struct lru_node {
    char*            key;      /**< Owned key copy. */
    void*            val;      /**< Owned value (see file brief). */
    struct lru_node* prev;     /**< Recency list: newer neighbor. */
    struct lru_node* next;     /**< Recency list: older neighbor. */
    struct lru_node* next_bkt; /**< Hash bucket chain. */
    size_t           h;        /**< Cached full hash of key. */
};

/** @brief LRU map instance: recency list + hash table under one mutex. */
struct lru {
    pthread_mutex_t   mtx;      /**< Guards everything below. */
    size_t            capacity; /**< Max live entries. */
    size_t            count;    /**< Current live entries. */
    lru_evict_fn      on_evict; /**< Displaced-value callback (may be NULL). */
    struct lru_node*  head; /**< Recency list: most recent. */
    struct lru_node*  tail; /**< Recency list: least recent. */
    struct lru_node** table;    /**< Bucket array. */
    size_t            table_cap; /**< Bucket count (power of two). */
};

/** @brief FNV-1a hash modulo the bucket count.
 *  @param s    NUL-terminated key, must not be NULL.
 *  @param cap  Hash table capacity (> 0).
 *  @return h(s) % cap, used as the bucket index. */
static size_t
fnv1a(const char* s, size_t cap)
{
    size_t h = 1469598103934665603ULL;
    while (*s) {
        h = (h ^ (unsigned char)*s++) * 1099511628211ULL;
    }
    return h % cap;
}

/** @brief Round up to a power of 2 (minimum 4, used as hash table initial/growth capacity).
 *  @param n  Desired capacity.
 *  @return Smallest power of 2 ≥ @p n, at least 4. */
static size_t
next_pow2(size_t n)
{
    size_t p = 4;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

lru_t*
lru_new(size_t capacity, lru_evict_fn on_evict)
{
    lru_t* lr = malloc(sizeof *lr);
    if (lr == NULL) {
        return NULL;
    }
    lr->mtx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    lr->capacity = capacity < 1 ? 1 : capacity;
    lr->count = 0;
    lr->on_evict = on_evict;
    lr->head = lr->tail = NULL;
    lr->table_cap = next_pow2(lr->capacity * 2);
    lr->table = calloc(lr->table_cap, sizeof *lr->table);
    if (lr->table == NULL) {
        free(lr);
        return NULL;
    }
    return lr;
}

/* @invariant caller holds lr->mtx. */

/** @brief Insert a node at the head of the recency list (mark as most recently used).
 *  @note Caller holds lr->mtx. */
static void
rec_link_head(lru_t* lr, struct lru_node* n)
{
    n->next = lr->head;
    n->prev = NULL;
    if (lr->head != NULL) {
        lr->head->prev = n;
    }
    lr->head = n;
    if (lr->tail == NULL) {
        lr->tail = n;
    }
}

/** @brief Detach a node from the recency list (relink neighbors, clear its own prev/next).
 *  @note Caller holds lr->mtx. */
static void
rec_unlink(lru_t* lr, struct lru_node* n)
{
    if (n->prev != NULL) {
        n->prev->next = n->next;
    } else {
        lr->head = n->next;
    }
    if (n->next != NULL) {
        n->next->prev = n->prev;
    } else {
        lr->tail = n->prev;
    }
    n->prev = n->next = NULL;
}

/** @brief Refresh node recency (no-op if already at head, else detach then reinsert at head).
 *  @note Caller holds lr->mtx. */
static void
rec_move_to_head(lru_t* lr, struct lru_node* n)
{
    if (lr->head == n) {
        return;
    }
    rec_unlink(lr, n);
    rec_link_head(lr, n);
}

/** @brief Look up a node by key in the hash bucket chain (exact strcmp match).
 *  @return Hit node; NULL when the table is empty or on a miss.
 *  @note Caller holds lr->mtx. */
static struct lru_node*
find_node(lru_t* lr, const char* key)
{
    struct lru_node* n;
    if (lr->table_cap == 0) {
        return NULL;
    }
    for (n = lr->table[fnv1a(key, lr->table_cap)]; n != NULL; n = n->next_bkt) {
        if (strcmp(n->key, key) == 0) {
            return n;
        }
    }
    return NULL;
}

/** @brief Hash a node and insert it at the head of the corresponding bucket chain.
 *  @note Caller holds lr->mtx. */
static void
bkt_insert(lru_t* lr, struct lru_node* n)
{
    n->h = fnv1a(n->key, lr->table_cap);
    n->next_bkt = lr->table[n->h];
    lr->table[n->h] = n;
}

/** @brief Remove a node from its bucket chain (pointer-to-pointer style; node must be in the table).
 *  @note Caller holds lr->mtx. */
static void
bkt_remove(lru_t* lr, struct lru_node* n)
{
    struct lru_node** pp = &lr->table[n->h];
    while (*pp != n) {
        pp = &(*pp)->next_bkt;
    }
    *pp = n->next_bkt;
    n->next_bkt = NULL;
}

/** @brief Free a node's key and the node itself; value belongs to the owner (handed back via on_evict), untouched here. */
static void
destroy_node(struct lru_node* n)
{
    free(n->key);
    free(n);
}

void
lru_free(lru_t* lr)
{
    struct lru_node* n = lr->head;
    while (n != NULL) {
        struct lru_node* next = n->next;
        if (lr->on_evict != NULL) {
            lr->on_evict(n->val);
        }
        destroy_node(n);
        n = next;
    }
    free(lr->table);
    free(lr);
}

void*
lru_get(lru_t* lr, const char* key)
{
    struct lru_node* n;
    void*            v = NULL;
    pthread_mutex_lock(&lr->mtx);
    n = find_node(lr, key);
    if (n != NULL) {
        rec_move_to_head(lr, n);
        v = n->val;
    }
    pthread_mutex_unlock(&lr->mtx);
    return v;
}

void
lru_put(lru_t* lr, const char* key, void* val)
{
    struct lru_node *n, *old = NULL;

    pthread_mutex_lock(&lr->mtx);
    old = find_node(lr, key);
    if (old != NULL) {
        void* oval = old->val;
        old->val = val;
        rec_move_to_head(lr, old);
        pthread_mutex_unlock(&lr->mtx);
        /* LRU owns values: hand the displaced old value to the owner. */
        if (lr->on_evict != NULL) {
            lr->on_evict(oval);
        }
        return;
    }
    char* kcopy = strdup(key);
    if (kcopy == NULL) {
        pthread_mutex_unlock(&lr->mtx);
        return;
    }

    n = malloc(sizeof *n);
    if (n == NULL) {
        pthread_mutex_unlock(&lr->mtx);
        free(kcopy);
        return;
    }
    n->key = kcopy;
    n->val = val;
    n->prev = n->next = n->next_bkt = NULL;

    if (lr->count == lr->capacity && lr->tail != NULL) {
        struct lru_node* victim = lr->tail;
        void*            vval = victim->val;
        rec_unlink(lr, victim);
        bkt_remove(lr, victim);
        destroy_node(victim);
        lr->count--;
        if (lr->on_evict != NULL) {
            lr->on_evict(vval);
        }
    }

    rec_link_head(lr, n);
    bkt_insert(lr, n);
    lr->count++;
    pthread_mutex_unlock(&lr->mtx);
}

int
lru_invalidate(lru_t* lr, const char* key)
{
    struct lru_node* n;
    int              found = 0;
    pthread_mutex_lock(&lr->mtx);
    n = find_node(lr, key);
    if (n != NULL) {
        rec_unlink(lr, n);
        bkt_remove(lr, n);
        void* val = n->val;
        destroy_node(n);
        lr->count--;
        found = 1;
        pthread_mutex_unlock(&lr->mtx);
        /* the LRU owns the value: hand it to the evict callback, matching
     * capacity-eviction and lru_free behavior. */
        if (lr->on_evict != NULL) {
            lr->on_evict(val);
        }
        return found;
    }
    pthread_mutex_unlock(&lr->mtx);
    return found;
}

size_t
lru_size(const lru_t* lr)
{
    pthread_mutex_lock(&((lru_t*)lr)->mtx);
    size_t c = lr->count;
    pthread_mutex_unlock(&((lru_t*)lr)->mtx);
    return c;
}
