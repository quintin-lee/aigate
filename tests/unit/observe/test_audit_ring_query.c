/** @file test_audit_ring_query.c
 *  @brief Unit tests for audit_logger in-memory live ring buffer querying.
 */
#include "audit_logger.h"
#include "run_tests.h"
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_audit_live_ring_query_recent)
{
    audit_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    audit_logger_t* logger = audit_logger_create(&cfg);
    TEST_ASSERT(logger != NULL, "logger create failed");

    /* Record 2 events */
    audit_event_t ev1;
    audit_event_init(&ev1);
    strncpy(ev1.trace_id, "tr-001", sizeof(ev1.trace_id) - 1);
    strncpy(ev1.model, "gpt-4o", sizeof(ev1.model) - 1);
    ev1.http_status = 200;
    ev1.severity = AUDIT_SEV_INFO;
    audit_logger_record(logger, &ev1);

    audit_event_t ev2;
    audit_event_init(&ev2);
    strncpy(ev2.trace_id, "tr-002", sizeof(ev2.trace_id) - 1);
    strncpy(ev2.model, "deepseek-r1", sizeof(ev2.model) - 1);
    ev2.http_status = 400;
    ev2.severity = AUDIT_SEV_VIOLATION;
    strncpy(ev2.violation_type, "JAILBREAK", sizeof(ev2.violation_type) - 1);
    audit_event_set_prompt(&ev2, "System override test", 100);
    audit_logger_record(logger, &ev2);
    audit_event_cleanup(&ev2);

    /* Query recent events */
    audit_live_event_t out_events[10];
    size_t             missed = 0;
    size_t             count = audit_logger_query_recent(logger, out_events, 10, 0, &missed);
    TEST_ASSERT(count == 2, "expected 2 events, got %zu", count);
    TEST_ASSERT(missed == 0, "missed should be 0");
    TEST_ASSERT(strcmp(out_events[0].trace_id, "tr-001") == 0, "first trace mismatch");
    TEST_ASSERT(strcmp(out_events[1].trace_id, "tr-002") == 0, "second trace mismatch");
    TEST_ASSERT(out_events[1].severity == AUDIT_SEV_VIOLATION, "severity mismatch");
    TEST_ASSERT(strcmp(out_events[1].prompt_snippet, "System override test") == 0,
                "prompt snippet mismatch");

    /* Test incremental fetch with after_seq */
    uint64_t last_seq = out_events[0].seq_id;
    count = audit_logger_query_recent(logger, out_events, 10, last_seq, &missed);
    TEST_ASSERT(count == 1, "expected 1 event with after_seq, got %zu", count);
    TEST_ASSERT(strcmp(out_events[0].trace_id, "tr-002") == 0, "incremental trace mismatch");

    audit_logger_destroy(logger);
}
