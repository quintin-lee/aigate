/** @file test_event_bus.c
 *  @brief Unit tests for in-memory pub-sub event bus.
 */
#include "event_bus.h"
#include "run_tests.h"
#include <string.h>
#include <stdio.h>

TEST_CASE(test_event_bus_lifecycle)
{
    event_bus_t* eb = event_bus_new();
    TEST_ASSERT(eb != NULL, "event_bus_new failed");
    event_bus_free(eb);
}

TEST_CASE(test_event_bus_sub_unsub)
{
    event_bus_t* eb = event_bus_new();
    TEST_ASSERT(eb != NULL, "event_bus_new failed");

    int sub_ids[MAX_EVENT_SUBSCRIBERS];
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        sub_ids[i] = event_bus_subscribe(eb);
        TEST_ASSERT(sub_ids[i] > 0, "subscribe failed at %d", i);
    }

    /* 9th subscriber should fail */
    int overflow = event_bus_subscribe(eb);
    TEST_ASSERT(overflow == -1, "overflow subscriber did not fail");

    /* Unsubscribe one and re-subscribe */
    event_bus_unsubscribe(eb, sub_ids[0]);
    int new_sub = event_bus_subscribe(eb);
    TEST_ASSERT(new_sub > 0, "resubscribe failed");

    event_bus_free(eb);
}

TEST_CASE(test_event_bus_publish_pop)
{
    event_bus_t* eb = event_bus_new();
    TEST_ASSERT(eb != NULL, "event_bus_new failed");

    int sub1 = event_bus_subscribe(eb);
    int sub2 = event_bus_subscribe(eb);
    TEST_ASSERT(sub1 > 0 && sub2 > 0, "subscribers creation failed");

    event_bus_publish_request(
        eb, 123, "gpt-4o", "openai", 200, 150000000ULL, 50, 20, 0.0005, "masked");

    event_item_t item1;
    int          rc1 = event_bus_pop(eb, sub1, &item1, 500);
    TEST_ASSERT(rc1 == 1, "pop item1 failed");
    TEST_ASSERT(item1.type == EVENT_REQUEST, "item1 type mismatch");
    TEST_ASSERT(strcmp(item1.event_name, "request") == 0, "item1 event_name mismatch");
    TEST_ASSERT(strstr(item1.payload, "\"model\":\"gpt-4o\"") != NULL, "item1 model missing");
    TEST_ASSERT(strstr(item1.payload, "\"provider\":\"openai\"") != NULL, "item1 provider missing");
    TEST_ASSERT(strstr(item1.payload, "\"status\":200") != NULL, "item1 status missing");
    TEST_ASSERT(strstr(item1.payload, "\"guardrail\":\"masked\"") != NULL,
                "item1 guardrail missing");
    TEST_ASSERT(strstr(item1.payload, "\"cached\":false") != NULL, "item1 cached missing");

    event_item_t item2;
    int          rc2 = event_bus_pop(eb, sub2, &item2, 500);
    TEST_ASSERT(rc2 == 1, "pop item2 failed");
    TEST_ASSERT(item2.type == EVENT_REQUEST, "item2 type mismatch");
    TEST_ASSERT(strstr(item2.payload, "\"key_id\":123") != NULL, "item2 key_id missing");

    /* Pop again on empty queue with 20ms timeout */
    event_item_t item3;
    int          rc3 = event_bus_pop(eb, sub1, &item3, 20);
    TEST_ASSERT(rc3 == 0, "pop on empty queue should return 0");

    event_bus_free(eb);
}

TEST_CASE(test_event_bus_overflow_drop)
{
    event_bus_t* eb = event_bus_new();
    TEST_ASSERT(eb != NULL, "event_bus_new failed");

    int sub = event_bus_subscribe(eb);
    TEST_ASSERT(sub > 0, "subscribe failed");

    /* Publish 70 events (capacity is 64) */
    char buf[64];
    for (int i = 0; i < 70; i++) {
        snprintf(buf, sizeof buf, "{\"seq\":%d}", i);
        event_bus_publish(eb, EVENT_REQUEST, "request", buf);
    }

    /* Oldest 6 events (0..5) were dropped, first popped should be seq 6 */
    event_item_t item;
    int          rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "pop after overflow failed");
    TEST_ASSERT(strstr(item.payload, "\"seq\":6") != NULL,
                "expected seq 6 after drop, got %s",
                item.payload);

    event_bus_free(eb);
}

TEST_CASE(test_event_bus_helpers)
{
    event_bus_t* eb = event_bus_new();
    TEST_ASSERT(eb != NULL, "event_bus_new failed");

    int sub = event_bus_subscribe(eb);
    TEST_ASSERT(sub > 0, "subscribe failed");

    event_bus_publish_cb(eb, "deepseek", "deepseek-v3", "CLOSED", "OPEN", "5xx errors");
    event_bus_publish_health(eb, "anthropic", "HEALTHY", 185, 200, "");
    event_bus_publish_budget(eb, "key", 42, "dev-team", 88.5, 88.5, 100.0);
    event_bus_publish_ping(eb);

    event_item_t item;
    int          rc;

    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "pop cb failed");
    TEST_ASSERT(item.type == EVENT_CIRCUIT_BREAKER, "type cb mismatch");
    TEST_ASSERT(strstr(item.payload, "\"new_state\":\"OPEN\"") != NULL, "new_state missing");

    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "pop health failed");
    TEST_ASSERT(item.type == EVENT_HEALTH_PROBE, "type health mismatch");
    TEST_ASSERT(strstr(item.payload, "\"status\":\"HEALTHY\"") != NULL, "status missing");

    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "pop budget failed");
    TEST_ASSERT(item.type == EVENT_BUDGET_ALERT, "type budget mismatch");
    TEST_ASSERT(strstr(item.payload, "\"percent\":88.5") != NULL, "percent missing");

    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "pop ping failed");
    TEST_ASSERT(item.type == EVENT_PING, "type ping mismatch");
    TEST_ASSERT(strstr(item.payload, "\"ts\":") != NULL, "ts missing");

    event_bus_free(eb);
}
