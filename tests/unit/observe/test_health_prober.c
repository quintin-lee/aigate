/** @file test_health_prober.c
 *  @brief Unit tests for active upstream health prober engine.
 */
#include "health_prober.h"
#include "event_bus.h"
#include "run_tests.h"
#include <string.h>
#include <stdlib.h>

TEST_CASE(test_health_prober_lifecycle)
{
    event_bus_t*     eb = event_bus_new();
    health_prober_t* hp = health_prober_new(NULL, NULL, eb, 60);
    TEST_ASSERT(hp != NULL, "health_prober_new failed");

    int start_rc = health_prober_start(hp);
    TEST_ASSERT(start_rc == 0, "health_prober_start failed");

    health_prober_stop(hp);
    health_prober_free(hp);
    event_bus_free(eb);
}

TEST_CASE(test_health_prober_state_transitions)
{
    event_bus_t* eb = event_bus_new();
    int          sub = event_bus_subscribe(eb);
    TEST_ASSERT(sub > 0, "event_bus_subscribe failed");

    health_prober_t* hp = health_prober_new(NULL, NULL, eb, 60);
    TEST_ASSERT(hp != NULL, "health_prober_new failed");

    /* 1. First probe 200 with 120ms -> HEALTHY */
    health_prober_record_result(
        hp, 1, "openai", "https://api.openai.com/v1", "openai", 200, 120, 0);

    event_item_t item;
    int          rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "expected health_probe event on initial status");
    TEST_ASSERT(item.type == EVENT_HEALTH_PROBE, "event type mismatch");
    TEST_ASSERT(strstr(item.payload, "\"status\":\"HEALTHY\"") != NULL, "status should be HEALTHY");

    /* 2. High latency 2200ms -> DEGRADED */
    health_prober_record_result(
        hp, 1, "openai", "https://api.openai.com/v1", "openai", 200, 2200, 0);
    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "expected health_probe event on degradation");
    TEST_ASSERT(strstr(item.payload, "\"status\":\"DEGRADED\"") != NULL,
                "status should be DEGRADED");

    /* 3. HTTP 401 Invalid Key -> DOWN immediately */
    health_prober_record_result(
        hp, 1, "openai", "https://api.openai.com/v1", "openai", 401, 100, 0);
    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "expected health_probe event on DOWN");
    TEST_ASSERT(strstr(item.payload, "\"status\":\"DOWN\"") != NULL, "status should be DOWN");

    /* 4. Recovered 200 with 95ms -> back to HEALTHY */
    health_prober_record_result(hp, 1, "openai", "https://api.openai.com/v1", "openai", 200, 95, 0);
    rc = event_bus_pop(eb, sub, &item, 100);
    TEST_ASSERT(rc == 1, "expected health_probe event on recovery");
    TEST_ASSERT(strstr(item.payload, "\"status\":\"HEALTHY\"") != NULL,
                "status should be recovered to HEALTHY");

    health_prober_free(hp);
    event_bus_free(eb);
}

TEST_CASE(test_health_prober_json_serialization)
{
    health_prober_t* hp = health_prober_new(NULL, NULL, NULL, 60);
    TEST_ASSERT(hp != NULL, "health_prober_new failed");

    health_prober_record_result(
        hp, 1, "openai", "https://api.openai.com/v1", "openai", 200, 110, 0);
    health_prober_record_result(
        hp, 2, "anthropic", "https://api.anthropic.com", "anthropic", 503, 1500, 0);
    /* 2nd 503 -> DOWN */
    health_prober_record_result(
        hp, 2, "anthropic", "https://api.anthropic.com", "anthropic", 503, 1600, 0);

    char* json_str = health_prober_to_json(hp);
    TEST_ASSERT(json_str != NULL, "json output NULL");
    TEST_ASSERT(strstr(json_str, "\"total\":2") != NULL, "total missing or incorrect");
    TEST_ASSERT(strstr(json_str, "\"healthy\":1") != NULL, "healthy missing or incorrect");
    TEST_ASSERT(strstr(json_str, "\"down\":1") != NULL, "down missing or incorrect");
    TEST_ASSERT(strstr(json_str, "\"name\":\"openai\"") != NULL, "openai missing");
    TEST_ASSERT(strstr(json_str, "\"name\":\"anthropic\"") != NULL, "anthropic missing");

    free(json_str);
    health_prober_free(hp);
}
