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

/** @brief FNV-1a 哈希并对桶数取模。
 *  @param s    NUL 结尾的键，不许 NULL。
 *  @param cap  哈希表容量（> 0）。
 *  @return h(s) % cap，用作桶下标。 */
static size_t
fnv1a(const char* s, size_t cap)
{
    size_t h = 1469598103934665603ULL;
    while (*s) {
        h = (h ^ (unsigned char)*s++) * 1099511628211ULL;
    }
    return h % cap;
}

/** @brief 向上取 2 的幂（最小 4，用作哈希表初始/扩容容量）。
 *  @param n  期望容量。
 *  @return ≥ @p n 的最小 2 的幂，至少为 4。 */
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

/** @brief 把节点插到新近度链表头（标记为最近使用）。
 *  @note 调用方持有 lr->mtx。 */
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

/** @brief 把节点从新近度链表摘除（前后驱重链，自身 prev/next 置空）。
 *  @note 调用方持有 lr->mtx。 */
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

/** @brief 刷新节点新近度（已在头则无操作，否则摘除后插头）。
 *  @note 调用方持有 lr->mtx。 */
static void
rec_move_to_head(lru_t* lr, struct lru_node* n)
{
    if (lr->head == n) {
        return;
    }
    rec_unlink(lr, n);
    rec_link_head(lr, n);
}

/** @brief 按键在哈希桶链中查找节点（strcmp 精确匹配）。
 *  @return 命中节点；表空或未命中返回 NULL。
 *  @note 调用方持有 lr->mtx。 */
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

/** @brief 计算节点哈希并头插进对应桶链。
 *  @note 调用方持有 lr->mtx。 */
static void
bkt_insert(lru_t* lr, struct lru_node* n)
{
    n->h = fnv1a(n->key, lr->table_cap);
    n->next_bkt = lr->table[n->h];
    lr->table[n->h] = n;
}

/** @brief 把节点从其桶链摘除（pointer-to-pointer 写法；要求节点确在表中）。
 *  @note 调用方持有 lr->mtx。 */
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

/** @brief 释放节点的 key 与节点本体；value 归 owner（经 on_evict 交还），此处不碰。 */
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
