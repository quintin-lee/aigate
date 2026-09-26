/** @file budget_enforce.c
 *  @brief In-memory and Redis monthly budget limit enforcer implementation.
 */
#include "budget_enforce.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define BUCKET_COUNT 1024

typedef struct key_spend_node {
    int64_t                key_id;
    double                 spent_cost;
    int64_t                spent_tokens;
    struct key_spend_node* next;
} key_spend_node_t;

typedef struct group_spend_node {
    int64_t                  group_id;
    double                   spent_cost;
    struct group_spend_node* next;
} group_spend_node_t;

struct budget_enforce_mgr {
    pg_store_t*         store;
    redis_pool_t*       redis;
    pthread_mutex_t     mtx;
    int                 current_ym;
    key_spend_node_t*   key_buckets[BUCKET_COUNT];
    group_spend_node_t* group_buckets[BUCKET_COUNT];
};

static int
get_current_year_month(void)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    return (tm_buf.tm_year + 1900) * 100 + (tm_buf.tm_mon + 1);
}

static inline size_t
hash_id(int64_t id)
{
    uint64_t x = (uint64_t)id;
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return (size_t)(x % BUCKET_COUNT);
}

static void
clear_buckets(budget_enforce_mgr_t* mgr)
{
    for (size_t i = 0; i < BUCKET_COUNT; i++) {
        key_spend_node_t* k = mgr->key_buckets[i];
        while (k != NULL) {
            key_spend_node_t* next = k->next;
            free(k);
            k = next;
        }
        mgr->key_buckets[i] = NULL;

        group_spend_node_t* g = mgr->group_buckets[i];
        while (g != NULL) {
            group_spend_node_t* next = g->next;
            free(g);
            g = next;
        }
        mgr->group_buckets[i] = NULL;
    }
}

budget_enforce_mgr_t*
budget_enforce_create(pg_store_t* store, redis_pool_t* redis)
{
    budget_enforce_mgr_t* mgr = calloc(1, sizeof(*mgr));
    if (mgr == NULL) {
        return NULL;
    }
    mgr->store = store;
    mgr->redis = redis;
    pthread_mutex_init(&mgr->mtx, NULL);
    mgr->current_ym = get_current_year_month();
    return mgr;
}

void
budget_enforce_destroy(budget_enforce_mgr_t* mgr)
{
    if (mgr == NULL) {
        return;
    }
    pthread_mutex_lock(&mgr->mtx);
    clear_buckets(mgr);
    pthread_mutex_unlock(&mgr->mtx);
    pthread_mutex_destroy(&mgr->mtx);
    free(mgr);
}

void
budget_enforce_reset(budget_enforce_mgr_t* mgr)
{
    if (mgr == NULL) {
        return;
    }
    pthread_mutex_lock(&mgr->mtx);
    clear_buckets(mgr);
    mgr->current_ym = get_current_year_month();
    pthread_mutex_unlock(&mgr->mtx);
}

int
budget_enforce_check(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    int64_t               group_id,
    double                key_cost_budget,
    int64_t               key_token_budget,
    double                group_cost_budget,
    char*                 err_msg,
    size_t                err_msg_sz)
{
    if (mgr == NULL) {
        return 0;
    }

    pthread_mutex_lock(&mgr->mtx);
    int now_ym = get_current_year_month();
    if (now_ym != mgr->current_ym) {
        clear_buckets(mgr);
        mgr->current_ym = now_ym;
    }

    /* 1. Check Key limits */
    if (key_id > 0 && (key_cost_budget > 0.0 || key_token_budget > 0)) {
        size_t h = hash_id(key_id);
        key_spend_node_t* curr = mgr->key_buckets[h];
        while (curr != NULL && curr->key_id != key_id) {
            curr = curr->next;
        }
        if (curr != NULL) {
            if (key_cost_budget > 0.0 && curr->spent_cost >= key_cost_budget) {
                if (err_msg != NULL && err_msg_sz > 0) {
                    snprintf(err_msg, err_msg_sz,
                             "Monthly cost budget of $%.2f exceeded for API key (spent: $%.2f)",
                             key_cost_budget, curr->spent_cost);
                }
                pthread_mutex_unlock(&mgr->mtx);
                return -1;
            }
            if (key_token_budget > 0 && curr->spent_tokens >= key_token_budget) {
                if (err_msg != NULL && err_msg_sz > 0) {
                    snprintf(err_msg, err_msg_sz,
                             "Monthly token budget of %ld tokens exceeded for API key (spent: %ld)",
                             (long)key_token_budget, (long)curr->spent_tokens);
                }
                pthread_mutex_unlock(&mgr->mtx);
                return -1;
            }
        }
    }

    /* 2. Check Group limit */
    if (group_id > 0 && group_cost_budget > 0.0) {
        size_t h = hash_id(group_id);
        group_spend_node_t* curr = mgr->group_buckets[h];
        while (curr != NULL && curr->group_id != group_id) {
            curr = curr->next;
        }
        if (curr != NULL) {
            if (curr->spent_cost >= group_cost_budget) {
                if (err_msg != NULL && err_msg_sz > 0) {
                    snprintf(err_msg, err_msg_sz,
                             "Monthly budget of $%.2f exceeded for group %ld (spent: $%.2f)",
                             group_cost_budget, (long)group_id, curr->spent_cost);
                }
                pthread_mutex_unlock(&mgr->mtx);
                return -1;
            }
        }
    }

    pthread_mutex_unlock(&mgr->mtx);
    return 0;
}

void
budget_enforce_record(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    int64_t               group_id,
    double                cost_usd,
    int64_t               tokens)
{
    if (mgr == NULL) {
        return;
    }

    pthread_mutex_lock(&mgr->mtx);
    int now_ym = get_current_year_month();
    if (now_ym != mgr->current_ym) {
        clear_buckets(mgr);
        mgr->current_ym = now_ym;
    }

    if (key_id > 0) {
        size_t h = hash_id(key_id);
        key_spend_node_t* curr = mgr->key_buckets[h];
        while (curr != NULL && curr->key_id != key_id) {
            curr = curr->next;
        }
        if (curr == NULL) {
            curr = calloc(1, sizeof(*curr));
            if (curr != NULL) {
                curr->key_id = key_id;
                curr->next = mgr->key_buckets[h];
                mgr->key_buckets[h] = curr;
            }
        }
        if (curr != NULL) {
            curr->spent_cost += cost_usd;
            curr->spent_tokens += tokens;
        }
    }

    if (group_id > 0) {
        size_t h = hash_id(group_id);
        group_spend_node_t* curr = mgr->group_buckets[h];
        while (curr != NULL && curr->group_id != group_id) {
            curr = curr->next;
        }
        if (curr == NULL) {
            curr = calloc(1, sizeof(*curr));
            if (curr != NULL) {
                curr->group_id = group_id;
                curr->next = mgr->group_buckets[h];
                mgr->group_buckets[h] = curr;
            }
        }
        if (curr != NULL) {
            curr->spent_cost += cost_usd;
        }
    }

    pthread_mutex_unlock(&mgr->mtx);
}

int
budget_enforce_get_key_usage(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    double*               out_cost,
    int64_t*              out_tokens)
{
    if (mgr == NULL || key_id <= 0) {
        return -1;
    }
    pthread_mutex_lock(&mgr->mtx);
    size_t h = hash_id(key_id);
    key_spend_node_t* curr = mgr->key_buckets[h];
    while (curr != NULL && curr->key_id != key_id) {
        curr = curr->next;
    }
    if (curr != NULL) {
        if (out_cost != NULL) *out_cost = curr->spent_cost;
        if (out_tokens != NULL) *out_tokens = curr->spent_tokens;
    } else {
        if (out_cost != NULL) *out_cost = 0.0;
        if (out_tokens != NULL) *out_tokens = 0;
    }
    pthread_mutex_unlock(&mgr->mtx);
    return 0;
}

int
budget_enforce_get_group_usage(
    budget_enforce_mgr_t* mgr,
    int64_t               group_id,
    double*               out_cost)
{
    if (mgr == NULL || group_id <= 0) {
        return -1;
    }
    pthread_mutex_lock(&mgr->mtx);
    size_t h = hash_id(group_id);
    group_spend_node_t* curr = mgr->group_buckets[h];
    while (curr != NULL && curr->group_id != group_id) {
        curr = curr->next;
    }
    if (curr != NULL) {
        if (out_cost != NULL) *out_cost = curr->spent_cost;
    } else {
        if (out_cost != NULL) *out_cost = 0.0;
    }
    pthread_mutex_unlock(&mgr->mtx);
    return 0;
}

int
budget_enforce_init_from_db(budget_enforce_mgr_t* mgr)
{
    if (mgr == NULL || mgr->store == NULL) {
        return 0;
    }
    const pg_ops_t* ops = pg_store_ops(mgr->store);
    if (ops == NULL || ops->query_cost == NULL) {
        return 0;
    }

    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    tm_buf.tm_mday = 1;
    tm_buf.tm_hour = 0;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    time_t month_start = timegm(&tm_buf);

    cost_row_t rows[256];
    int n = 0;
    if (ops->query_cost(ops->ctx, (long)month_start, (long)(now + 86400), rows, 256, &n) == 0) {
        for (int i = 0; i < n; i++) {
            budget_enforce_record(mgr, 0, rows[i].group_id, 0.0, rows[i].prompt + rows[i].completion);
        }
    }
    return 0;
}
