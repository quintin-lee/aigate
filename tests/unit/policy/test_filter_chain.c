#include "run_tests.h"
#include "policy/filter_chain.h"
#include "policy/guardrails.h"
#include "policy/prompt_template.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

static void
test_filter_chain_noop(void)
{
    aigate_core ac = {0};
    const char* raw_json =
        "{\"model\":\"m1\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    json_t*            jbody = json_loads(raw_json, 0, NULL);
    aigate_request_ctx rq = {
        .body = raw_json,
        .body_len = strlen(raw_json),
    };
    aigate_response_ctx rc = {0};

    chat_req_t q = {
        .ac = &ac,
        .rq = &rq,
        .rc = &rc,
        .jbody = jbody,
        .eff_body = raw_json,
        .eff_len = strlen(raw_json),
    };

    filter_action_t act = filter_chain_execute_inbound(&q);
    TEST_ASSERT(act == FILTER_CONTINUE, "noop returns continue");
    TEST_ASSERT(q.eff_body == raw_json, "body untouched");

    json_decref(jbody);
}

static void
test_filter_chain_prompt_injection(void)
{
    aigate_core ac = {0};
    const char* raw_json =
        "{\"model\":\"gpt-test\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    json_t*            jbody = json_loads(raw_json, 0, NULL);
    aigate_request_ctx rq = {
        .body = raw_json,
        .body_len = strlen(raw_json),
    };
    aigate_response_ctx rc = {0};

    chat_req_t q = {
        .ac = &ac,
        .rq = &rq,
        .rc = &rc,
        .jbody = jbody,
        .model = "gpt-test",
        .eff_body = raw_json,
        .eff_len = strlen(raw_json),
    };
    snprintf(q.krec.name, sizeof(q.krec.name), "corp-key");
    snprintf(q.krec.system_prompt,
             sizeof(q.krec.system_prompt),
             "Corporate rule for ${key_name} on ${model}");
    q.krec.prompt_mode = PROMPT_MODE_PREPEND;

    filter_action_t act = filter_chain_execute_inbound(&q);
    TEST_ASSERT(act == FILTER_CONTINUE, "returns continue");
    TEST_ASSERT(q.eff_body != raw_json, "body was modified");
    TEST_ASSERT(q.sanitized_body != NULL, "sanitized_body populated");

    json_t* res = json_loads((const char*)q.eff_body, 0, NULL);
    TEST_ASSERT(res != NULL, "valid json produced");
    json_t*     msgs = json_object_get(res, "messages");
    const char* sys_c = json_string_value(json_object_get(json_array_get(msgs, 0), "content"));
    TEST_ASSERT(strcmp(sys_c, "Corporate rule for corp-key on gpt-test") == 0,
                "system prompt correctly expanded: %s",
                sys_c);

    json_decref(res);
    free(q.sanitized_body);
    json_decref(jbody);
}

void
test_filter_chain_suite(void)
{
    TEST_CASE(test_filter_chain_noop);
    TEST_CASE(test_filter_chain_prompt_injection);
}
