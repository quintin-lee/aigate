#include "run_tests.h"
#include "policy/filter_chain.h"
#include "policy/watermark_engine.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_watermark_pipeline_openai_outbound)
{
    aigate_core ac = {0};
    const char* resp_json =
        "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion\",\"created\":1791552000,"
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":"
        "\"This is a strictly confidential intelligence report.\"},\"finish_reason\":\"stop\"}]}";

    chat_req_t q = {
        .ac = &ac,
    };
    q.krec.key_id = 7788;
    q.krec.watermark_enabled = 1;
    strncpy(
        q.trace_ctx.trace_id, "4bf92f3577b34da6a3ce929d0e0e4736", sizeof(q.trace_ctx.trace_id) - 1);

    char*           out_body = NULL;
    size_t          out_len = 0;
    filter_action_t act =
        filter_chain_execute_outbound(&q, resp_json, strlen(resp_json), &out_body, &out_len);
    TEST_ASSERT(act == FILTER_CONTINUE, "Expected FILTER_CONTINUE");
    TEST_ASSERT(out_body != NULL, "Expected modified out_body with watermark injected");
    TEST_ASSERT(out_len > strlen(resp_json),
                "Expected out_len to be larger due to zero-width UTF-8 bytes");

    /* 解析 out_body 并验证 choices[0].message.content 包含可解码的水印 */
    json_t* res = json_loads(out_body, 0, NULL);
    TEST_ASSERT(res != NULL, "out_body must be valid JSON");
    json_t*     choices = json_object_get(res, "choices");
    json_t*     choice0 = json_array_get(choices, 0);
    json_t*     msg = json_object_get(choice0, "message");
    const char* content = json_string_value(json_object_get(msg, "content"));
    TEST_ASSERT(content != NULL, "content must exist");

    watermark_payload_t decoded;
    memset(&decoded, 0, sizeof decoded);
    int drc = watermark_decode(content, strlen(content), &decoded);
    TEST_ASSERT(drc == 0, "watermark_decode must succeed on injected content");
    TEST_ASSERT(decoded.crc_valid == true, "CRC must be valid");
    TEST_ASSERT(decoded.key_id == 7788, "key_id must match 7788, got %u", decoded.key_id);

    json_decref(res);
    free(out_body);
}

TEST_CASE(test_watermark_pipeline_anthropic_outbound)
{
    aigate_core ac = {0};
    const char* resp_json =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"Top secret corporate strategy document.\"}],"
        "\"model\":\"claude-3-5-sonnet\"}";

    chat_req_t q = {
        .ac = &ac,
    };
    q.krec.key_id = 9911;
    q.krec.watermark_enabled = 1;
    strncpy(
        q.trace_ctx.trace_id, "abcdef1234567890abcdef1234567890", sizeof(q.trace_ctx.trace_id) - 1);

    char*           out_body = NULL;
    size_t          out_len = 0;
    filter_action_t act =
        filter_chain_execute_outbound(&q, resp_json, strlen(resp_json), &out_body, &out_len);
    TEST_ASSERT(act == FILTER_CONTINUE, "Expected FILTER_CONTINUE");
    TEST_ASSERT(out_body != NULL, "Expected out_body with watermark for anthropic response");

    json_t* res = json_loads(out_body, 0, NULL);
    TEST_ASSERT(res != NULL, "out_body must be valid JSON");
    json_t*     content_arr = json_object_get(res, "content");
    json_t*     block0 = json_array_get(content_arr, 0);
    const char* text_val = json_string_value(json_object_get(block0, "text"));
    TEST_ASSERT(text_val != NULL, "text field must exist");

    watermark_payload_t decoded;
    int                 drc = watermark_decode(text_val, strlen(text_val), &decoded);
    TEST_ASSERT(drc == 0 && decoded.crc_valid, "watermark_decode must succeed on anthropic block");
    TEST_ASSERT(decoded.key_id == 9911, "key_id must match 9911, got %u", decoded.key_id);

    json_decref(res);
    free(out_body);
}
