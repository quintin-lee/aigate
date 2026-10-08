/** @file test_audit_logger.c
 *  @brief Unit tests for audit logging, data models, and dual-channel pipeline.
 */
#include "audit_logger.h"
#include "run_tests.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_audit_event_serialization_and_snapshots)
{
    /* 1. 测试 INFO 级别：正常请求纯元数据，不携带 prompt 快照 */
    audit_event_t ev_info;
    audit_event_init(&ev_info);
    strncpy(ev_info.trace_id, "00-trace-info-01", sizeof(ev_info.trace_id) - 1);
    ev_info.timestamp_ms = 1760000000123LL;
    ev_info.key_id = 42;
    strncpy(ev_info.client_ip, "192.168.1.50", sizeof(ev_info.client_ip) - 1);
    strncpy(ev_info.model, "gpt-4o", sizeof(ev_info.model) - 1);
    strncpy(ev_info.provider, "openai", sizeof(ev_info.provider) - 1);
    ev_info.http_status = 200;
    ev_info.prompt_tokens = 120;
    ev_info.completion_tokens = 80;
    ev_info.latency_ns = 250000000ULL; /* 250ms */
    ev_info.severity = AUDIT_SEV_INFO;

    char* ndjson_info = audit_event_to_ndjson(&ev_info);
    TEST_ASSERT(ndjson_info != NULL, "ndjson_info serialization returned NULL");
    TEST_ASSERT(strstr(ndjson_info, "\"trace_id\":\"00-trace-info-01\"") != NULL,
                "trace_id missing");
    TEST_ASSERT(strstr(ndjson_info, "\"severity\":\"INFO\"") != NULL, "severity INFO missing");
    TEST_ASSERT(strstr(ndjson_info, "\"status\":200") != NULL, "status 200 missing");
    TEST_ASSERT(strstr(ndjson_info, "\"prompt\"") == NULL,
                "INFO level must NOT contain prompt snapshot");
    free(ndjson_info);
    audit_event_cleanup(&ev_info);

    /* 2. 测试 VIOLATION 级别：护栏违规拦截，全量上下文截断现场保留 */
    audit_event_t ev_violation;
    audit_event_init(&ev_violation);
    strncpy(ev_violation.trace_id, "00-trace-violation-02", sizeof(ev_violation.trace_id) - 1);
    ev_violation.timestamp_ms = 1760000000456LL;
    ev_violation.key_id = 42;
    strncpy(ev_violation.client_ip, "10.0.0.99", sizeof(ev_violation.client_ip) - 1);
    strncpy(ev_violation.model, "claude-3-5-sonnet", sizeof(ev_violation.model) - 1);
    strncpy(ev_violation.provider, "anthropic", sizeof(ev_violation.provider) - 1);
    ev_violation.http_status = 400;
    ev_violation.severity = AUDIT_SEV_VIOLATION;
    strncpy(ev_violation.violation_type, "pii_leak", sizeof(ev_violation.violation_type) - 1);
    strncpy(ev_violation.rule_detail,
            "detected phone_number regex",
            sizeof(ev_violation.rule_detail) - 1);
    audit_event_set_prompt(&ev_violation, "我的手机号是 13800138000，请帮我查询订单", 4096);

    char* ndjson_viol = audit_event_to_ndjson(&ev_violation);
    TEST_ASSERT(ndjson_viol != NULL, "ndjson_viol serialization returned NULL");
    TEST_ASSERT(strstr(ndjson_viol, "\"severity\":\"VIOLATION\"") != NULL,
                "severity VIOLATION missing");
    TEST_ASSERT(strstr(ndjson_viol, "\"type\":\"pii_leak\"") != NULL, "violation type missing");
    TEST_ASSERT(strstr(ndjson_viol, "13800138000") != NULL, "prompt snapshot missing");
    free(ndjson_viol);

    /* 3. 验证适配器格式化卡片 (Feishu, DingTalk, WeChat Work) */
    char* feishu_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_FEISHU);
    TEST_ASSERT(feishu_card != NULL, "feishu payload returned NULL");
    TEST_ASSERT(strstr(feishu_card, "\"msg_type\":\"interactive\"") != NULL,
                "feishu msg_type missing");
    TEST_ASSERT(strstr(feishu_card, "13800138000") != NULL, "feishu prompt missing");
    free(feishu_card);

    char* ding_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_DINGTALK);
    TEST_ASSERT(ding_card != NULL, "dingtalk payload returned NULL");
    TEST_ASSERT(strstr(ding_card, "\"msgtype\":\"markdown\"") != NULL, "dingtalk msgtype missing");
    free(ding_card);

    char* wx_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_WECHAT_WORK);
    TEST_ASSERT(wx_card != NULL, "wechat_work payload returned NULL");
    TEST_ASSERT(strstr(wx_card, "\"msgtype\":\"markdown\"") != NULL, "wechat_work msgtype missing");
    free(wx_card);

    audit_event_cleanup(&ev_violation);
}
