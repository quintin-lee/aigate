/** @file budget_enforce.h
 *  @brief Monthly budget limit enforcer for API Keys and Groups.
 */
#ifndef AIGATE_BUDGET_ENFORCE_H
#define AIGATE_BUDGET_ENFORCE_H

#include <stddef.h>
#include <stdint.h>
#include "pg_store.h"
#include "redis_pool.h"

typedef struct budget_enforce_mgr budget_enforce_mgr_t;

/** @brief Create a budget enforcer instance.
 *  @param store Optional PostgreSQL persistence store (for initial boot sync).
 *  @param redis Optional Redis pool (for cluster-wide sync).
 */
budget_enforce_mgr_t* budget_enforce_create(pg_store_t* store, redis_pool_t* redis);

/** @brief Destroy budget enforcer and free resources. */
void budget_enforce_destroy(budget_enforce_mgr_t* mgr);

/** @brief Synchronize month-to-date usage from PostgreSQL at startup. */
int budget_enforce_init_from_db(budget_enforce_mgr_t* mgr);

/** @brief Fast in-memory budget check against key and group limits.
 *  @return 0 if allowed, -1 if budget exceeded (with err_msg populated).
 */
int budget_enforce_check(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    int64_t               group_id,
    double                key_cost_budget,
    int64_t               key_token_budget,
    double                group_cost_budget,
    char*                 err_msg,
    size_t                err_msg_sz);

/** @brief Record incremental usage (cost and tokens) for key and group. */
void budget_enforce_record(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    int64_t               group_id,
    double                cost_usd,
    int64_t               tokens);

/** @brief Reset in-memory budget counters (e.g. for testing or forced rollover). */
void budget_enforce_reset(budget_enforce_mgr_t* mgr);

/** @brief Query current in-memory usage for a key. */
int budget_enforce_get_key_usage(
    budget_enforce_mgr_t* mgr,
    int64_t               key_id,
    double*               out_cost,
    int64_t*              out_tokens);

/** @brief Query current in-memory usage for a group. */
int budget_enforce_get_group_usage(
    budget_enforce_mgr_t* mgr,
    int64_t               group_id,
    double*               out_cost);

/** @brief Set/update monthly budget limit for a group. */
void budget_enforce_set_group_budget(
    budget_enforce_mgr_t* mgr,
    int64_t               group_id,
    double                budget_usd);

#endif /* AIGATE_BUDGET_ENFORCE_H */
