/** @file test_circuit_breaker.c
 *  @brief Unit tests for circuit breaker state machine & concurrency.
 */
#include "circuit_breaker.h"
#include "run_tests.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static time_t g_fake_time = 1000000;

static time_t
fake_time_provider(void)
{
    return g_fake_time;
}

TEST_CASE(test_cb_normal_traffic)
{
    circuit_breaker_t* cb = cb_create();
    TEST_ASSERT(cb != NULL, "cb_create");

    /* Initial state for unvisited target */
    TEST_ASSERT(cb_get_state(cb, "gpt-4", "http://ep1") == CB_CLOSED, "initial state closed");
    TEST_ASSERT(cb_allow_request(cb, "gpt-4", "http://ep1") == true, "allow initial request");

    /* Success keeps it closed */
    cb_record_success(cb, "gpt-4", "http://ep1");
    TEST_ASSERT(cb_get_state(cb, "gpt-4", "http://ep1") == CB_CLOSED, "still closed after success");
    TEST_ASSERT(strcmp(cb_state_to_str(CB_CLOSED), "closed") == 0, "state_to_str closed");

    cb_destroy(cb);
}

TEST_CASE(test_cb_tripping_on_consecutive_failures)
{
    circuit_breaker_t* cb = cb_create();
    cb_set_params(cb, 3, 30);
    g_fake_time = 1000;
    cb_set_time_fn(cb, fake_time_provider);

    /* 400 Bad Request is a client error; should NOT trip circuit breaker */
    cb_record_failure(cb, "claude", "http://ep1", 400);
    cb_record_failure(cb, "claude", "http://ep1", 400);
    cb_record_failure(cb, "claude", "http://ep1", 400);
    TEST_ASSERT(cb_get_state(cb, "claude", "http://ep1") == CB_CLOSED, "400 does not trip");

    /* 1st failover failure: 500 */
    cb_record_failure(cb, "claude", "http://ep1", 500);
    TEST_ASSERT(cb_get_state(cb, "claude", "http://ep1") == CB_CLOSED, "1 failure still closed");
    TEST_ASSERT(cb_allow_request(cb, "claude", "http://ep1") == true, "allowed after 1 failure");

    /* Success resets consecutive failures */
    cb_record_success(cb, "claude", "http://ep1");

    /* Now test 3 consecutive failures: 502, 429, 0 (network timeout) */
    cb_record_failure(cb, "claude", "http://ep1", 502);
    cb_record_failure(cb, "claude", "http://ep1", 429);
    TEST_ASSERT(cb_get_state(cb, "claude", "http://ep1") == CB_CLOSED, "2 failures still closed");

    cb_record_failure(cb, "claude", "http://ep1", 0);
    /* Circuit should now be tripped to OPEN */
    TEST_ASSERT(cb_get_state(cb, "claude", "http://ep1") == CB_OPEN, "tripped to open after 3 failures");
    TEST_ASSERT(cb_allow_request(cb, "claude", "http://ep1") == false, "traffic blocked when open");
    TEST_ASSERT(cb_get_open_until(cb, "claude", "http://ep1") == 1000 + 30, "open_until set to now + 30");
    TEST_ASSERT(strcmp(cb_state_to_str(CB_OPEN), "open") == 0, "state_to_str open");

    cb_destroy(cb);
}

TEST_CASE(test_cb_cooloff_and_half_open_probe_success)
{
    circuit_breaker_t* cb = cb_create();
    cb_set_params(cb, 3, 30);
    g_fake_time = 1000;
    cb_set_time_fn(cb, fake_time_provider);

    /* Trip to OPEN at t = 1000 */
    cb_record_failure(cb, "model1", "http://ep1", 500);
    cb_record_failure(cb, "model1", "http://ep1", 500);
    cb_record_failure(cb, "model1", "http://ep1", 500);
    TEST_ASSERT(cb_get_state(cb, "model1", "http://ep1") == CB_OPEN, "tripped");

    /* At t = 1020 (< 1030): still OPEN */
    g_fake_time = 1020;
    TEST_ASSERT(cb_get_state(cb, "model1", "http://ep1") == CB_OPEN, "still open at 1020");
    TEST_ASSERT(cb_allow_request(cb, "model1", "http://ep1") == false, "blocked at 1020");

    /* At t = 1030: cool-off elapsed -> transitions to HALF_OPEN */
    g_fake_time = 1030;
    TEST_ASSERT(cb_get_state(cb, "model1", "http://ep1") == CB_HALF_OPEN, "transitions to half_open");
    TEST_ASSERT(strcmp(cb_state_to_str(CB_HALF_OPEN), "half_open") == 0, "state_to_str half_open");

    /* First probe is allowed */
    TEST_ASSERT(cb_allow_request(cb, "model1", "http://ep1") == true, "probe allowed");
    /* Concurrent request during probe is rejected */
    TEST_ASSERT(cb_allow_request(cb, "model1", "http://ep1") == false, "concurrent request blocked");

    /* Probe succeeds */
    cb_record_success(cb, "model1", "http://ep1");
    TEST_ASSERT(cb_get_state(cb, "model1", "http://ep1") == CB_CLOSED, "recovered to closed");
    TEST_ASSERT(cb_allow_request(cb, "model1", "http://ep1") == true, "subsequent request allowed");

    cb_destroy(cb);
}

TEST_CASE(test_cb_probe_failure_trips_back_to_open)
{
    circuit_breaker_t* cb = cb_create();
    cb_set_params(cb, 3, 30);
    g_fake_time = 1000;
    cb_set_time_fn(cb, fake_time_provider);

    /* Trip to OPEN at t = 1000 */
    cb_record_failure(cb, "model2", "http://ep2", 503);
    cb_record_failure(cb, "model2", "http://ep2", 503);
    cb_record_failure(cb, "model2", "http://ep2", 503);
    TEST_ASSERT(cb_get_state(cb, "model2", "http://ep2") == CB_OPEN, "open");

    /* Advance to half-open */
    g_fake_time = 1030;
    TEST_ASSERT(cb_allow_request(cb, "model2", "http://ep2") == true, "probe allowed");

    /* Probe fails with 504 */
    cb_record_failure(cb, "model2", "http://ep2", 504);
    TEST_ASSERT(cb_get_state(cb, "model2", "http://ep2") == CB_OPEN, "tripped back to open");
    TEST_ASSERT(cb_get_open_until(cb, "model2", "http://ep2") == 1030 + 30, "new open_until 1060");
    TEST_ASSERT(cb_allow_request(cb, "model2", "http://ep2") == false, "blocked");

    /* Reset method */
    cb_reset(cb);
    TEST_ASSERT(cb_get_state(cb, "model2", "http://ep2") == CB_CLOSED, "reset cleans entries");

    cb_destroy(cb);
}

struct thread_arg {
    circuit_breaker_t* cb;
    int                id;
};

static void*
cb_thread_worker(void* v)
{
    struct thread_arg* arg = v;
    char               ep[64];
    snprintf(ep, sizeof(ep), "http://ep-%d", arg->id % 4);

    for (int i = 0; i < 200; i++) {
        bool allowed = cb_allow_request(arg->cb, "m", ep);
        if (allowed) {
            if ((i % 5) == 0) {
                cb_record_failure(arg->cb, "m", ep, 500);
            } else {
                cb_record_success(arg->cb, "m", ep);
            }
        }
    }
    return NULL;
}

TEST_CASE(test_cb_concurrency_stress)
{
    circuit_breaker_t* cb = cb_create();
    cb_set_params(cb, 3, 0); /* instant cooloff for stress */

    pthread_t         threads[8];
    struct thread_arg args[8];

    for (int i = 0; i < 8; i++) {
        args[i].cb = cb;
        args[i].id = i;
        pthread_create(&threads[i], NULL, cb_thread_worker, &args[i]);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    cb_destroy(cb);
}
