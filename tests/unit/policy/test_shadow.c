/** @file test_shadow.c
 *  @brief Unit tests for traffic shadowing, canary rules, queues, and evaluation cache.
 */
#include "policy/shadow.h"
#include "run_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_shadow_rule_matching_and_sampling)
{
    shadow_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.id = 1;
    strncpy(rule.source_model, "gpt-4o", sizeof(rule.source_model) - 1);
    strncpy(rule.target_model, "deepseek-chat", sizeof(rule.target_model) - 1);
    rule.mode = TRAFFIC_MODE_SHADOW;
    rule.sample_rate = 1.0;
    rule.enabled = true;
    strncpy(rule.header_match, "x-env: test", sizeof(rule.header_match) - 1);

    /* Matching source model and header */
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: test") == true,
                "rule matches model and header");
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-3.5-turbo", "x-env: test") == false,
                "mismatched model fails");
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: prod") == false,
                "mismatched header fails");

    /* Disabled rule */
    rule.enabled = false;
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "x-env: test") == false,
                "disabled rule fails");

    /* Wildcard or empty header match matches all */
    rule.enabled = true;
    rule.header_match[0] = '\0';
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", NULL) == true,
                "empty header matches null header");
    TEST_ASSERT(shadow_rule_matches(&rule, "gpt-4o", "anything: value") == true,
                "empty header matches any header");

    /* Sampling check */
    rule.sample_rate = 1.0;
    TEST_ASSERT(shadow_rule_should_sample(&rule) == true, "sample rate 1.0 always samples");
    rule.sample_rate = 0.0;
    TEST_ASSERT(shadow_rule_should_sample(&rule) == false, "sample rate 0.0 never samples");
}

TEST_CASE(test_shadow_queue_push_pop_overflow)
{
    shadow_queue_t* q = shadow_queue_create(4);
    TEST_ASSERT(q != NULL, "queue created");

    shadow_task_t t1;
    memset(&t1, 0, sizeof(t1));
    strncpy(t1.eval_id, "eval-001", sizeof(t1.eval_id) - 1);

    TEST_ASSERT(shadow_queue_push(q, &t1) == true, "push 1 ok");
    TEST_ASSERT(shadow_queue_count(q) == 1, "count is 1");

    /* Fill to capacity */
    for (int i = 2; i <= 4; i++) {
        shadow_task_t t;
        memset(&t, 0, sizeof(t));
        snprintf(t.eval_id, sizeof(t.eval_id), "eval-00%d", i);
        TEST_ASSERT(shadow_queue_push(q, &t) == true, "push ok");
    }
    TEST_ASSERT(shadow_queue_count(q) == 4, "queue full at 4");

    /* Push when full: non-blocking drop */
    shadow_task_t extra;
    memset(&extra, 0, sizeof(extra));
    strncpy(extra.eval_id, "eval-overflow", sizeof(extra.eval_id) - 1);
    TEST_ASSERT(shadow_queue_push(q, &extra) == false, "push on full queue returns false");
    TEST_ASSERT(shadow_queue_dropped(q) == 1, "dropped counter incremented");

    /* Pop FIFO */
    shadow_task_t popped;
    TEST_ASSERT(shadow_queue_pop(q, &popped, 100) == true, "pop succeeds");
    TEST_ASSERT(strcmp(popped.eval_id, "eval-001") == 0, "FIFO ordering verified");
    TEST_ASSERT(shadow_queue_count(q) == 3, "count decremented to 3");

    shadow_queue_destroy(q);
}

TEST_CASE(test_shadow_eval_cache_circular_and_stats)
{
    shadow_eval_cache_t* cache = shadow_eval_cache_create(5);
    TEST_ASSERT(cache != NULL, "cache created");

    for (int i = 1; i <= 7; i++) {
        shadow_eval_item_t item;
        memset(&item, 0, sizeof(item));
        snprintf(item.eval_id, sizeof(item.eval_id), "eval-%d", i);
        item.primary_latency_ms = 100.0;
        item.shadow_latency_ms = 40.0;
        item.primary_cost_usd = 0.01;
        item.shadow_cost_usd = 0.002;
        item.shadow_http_status = 200;
        shadow_eval_cache_record(cache, &item);
    }

    TEST_ASSERT(shadow_eval_cache_count(cache) == 5, "capped at capacity 5");

    shadow_eval_item_t list[5];
    int                n = shadow_eval_cache_get_recent(cache, list, 5);
    TEST_ASSERT(n == 5, "retrieved 5 items");
    /* Most recent first */
    TEST_ASSERT(strcmp(list[0].eval_id, "eval-7") == 0, "most recent item is eval-7");
    TEST_ASSERT(strcmp(list[4].eval_id, "eval-3") == 0, "oldest remaining is eval-3");

    shadow_stats_t stats;
    shadow_eval_cache_get_stats(cache, &stats);
    TEST_ASSERT(stats.total_evaluated == 7, "total evaluated is 7");
    TEST_ASSERT(stats.successful_shadow == 7, "all 7 successful");
    TEST_ASSERT(stats.cost_saved_usd > 0.05, "cost saved recorded correctly");

    shadow_eval_cache_destroy(cache);
}
