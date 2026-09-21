/** @file test_provider_deepseek.c
 *  @brief Unit tests for DeepSeek reasoning_content preservation and prompt cache token metering.
 */
#include "run_tests.h"
#include "provider_adapter.h"
#include "provider_openai.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_deepseek_reasoning_non_streaming)
{
    const char* raw_resp = "{\"id\":\"chatcmpl-ds-test\","
                           "\"object\":\"chat.completion\","
                           "\"choices\":[{"
                           "\"index\":0,"
                           "\"message\":{"
                           "\"role\":\"assistant\","
                           "\"content\":\"The result is 42.\","
                           "\"reasoning_content\":\"Let me ponder the universe and everything...\""
                           "},"
                           "\"finish_reason\":\"stop\""
                           "}],"
                           "\"usage\":{"
                           "\"prompt_tokens\":120,"
                           "\"completion_tokens\":60,"
                           "\"total_tokens\":180,"
                           "\"prompt_cache_hit_tokens\":95,"
                           "\"prompt_cache_miss_tokens\":25"
                           "}}";

    int    status = 0;
    char*  out_body = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0, cached_tok = 0;

    int rc = g_provider_openai.parse_chat_response(raw_resp,
                                                   strlen(raw_resp),
                                                   "deepseek-r1",
                                                   &status,
                                                   &out_body,
                                                   &out_len,
                                                   &ptok,
                                                   &ctok,
                                                   &cached_tok);

    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(status == 200, "status 200");
    TEST_ASSERT(ptok == 120, "prompt tokens 120, got %ld", ptok);
    TEST_ASSERT(ctok == 60, "completion tokens 60, got %ld", ctok);
    TEST_ASSERT(cached_tok == 95, "cached tokens 95, got %ld", cached_tok);
    TEST_ASSERT(out_body != NULL, "out_body allocated");

    json_t* parsed = json_loads(out_body, 0, NULL);
    TEST_ASSERT(parsed != NULL, "parsed json valid");
    if (parsed != NULL) {
        json_t* choices = json_object_get(parsed, "choices");
        TEST_ASSERT(choices && json_is_array(choices), "choices array");
        json_t* c0 = json_array_get(choices, 0);
        json_t* msg = json_object_get(c0, "message");
        TEST_ASSERT(msg && json_is_object(msg), "message object");
        json_t* jreas = json_object_get(msg, "reasoning_content");
        TEST_ASSERT(jreas && json_is_string(jreas), "reasoning_content preserved");
        if (jreas && json_is_string(jreas)) {
            TEST_ASSERT(strcmp(json_string_value(jreas),
                               "Let me ponder the universe and everything...") == 0,
                        "reasoning text matches");
        }
        json_decref(parsed);
    }
    free(out_body);
}

TEST_CASE(test_openai_cached_tokens_details)
{
    const char* raw_resp =
        "{\"id\":\"chatcmpl-oai-cached\","
        "\"object\":\"chat.completion\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"}}],"
        "\"usage\":{"
        "\"prompt_tokens\":1000,"
        "\"completion_tokens\":50,"
        "\"total_tokens\":1050,"
        "\"prompt_tokens_details\":{"
        "\"cached_tokens\":800"
        "}}"
        "}";

    int    status = 0;
    char*  out_body = NULL;
    size_t out_len = 0;
    long   ptok = 0, ctok = 0, cached_tok = 0;

    int rc = g_provider_openai.parse_chat_response(raw_resp,
                                                   strlen(raw_resp),
                                                   "gpt-4o",
                                                   &status,
                                                   &out_body,
                                                   &out_len,
                                                   &ptok,
                                                   &ctok,
                                                   &cached_tok);

    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(ptok == 1000, "ptok 1000");
    TEST_ASSERT(ctok == 50, "ctok 50");
    TEST_ASSERT(cached_tok == 800, "cached tokens 800, got %ld", cached_tok);
    free(out_body);
}

typedef struct {
    char   written[8192];
    size_t written_len;
    int    status;
} dummy_sink_t;

static int
dummy_set_hdr(void* impl, const char* name, const char* value)
{
    (void)impl;
    (void)name;
    (void)value;
    return 0;
}

static int
dummy_write(void* impl, const void* buf, size_t len, bool fin)
{
    (void)fin;
    dummy_sink_t* s = impl;
    if (s->written_len + len < sizeof(s->written)) {
        memcpy(s->written + s->written_len, buf, len);
        s->written_len += len;
        s->written[s->written_len] = '\0';
    }
    return 0;
}

TEST_CASE(test_deepseek_streaming_reasoning_and_cache)
{
    dummy_sink_t sink;
    memset(&sink, 0, sizeof sink);

    aigate_response_ctx rc;
    memset(&rc, 0, sizeof rc);
    rc.impl = &sink;
    rc.set_header = dummy_set_hdr;
    rc.write = dummy_write;

    stream_bridge_t* b = g_provider_openai.stream_bridge_new(&rc, "deepseek-r1");
    TEST_ASSERT(b != NULL, "bridge new ok");

    const char* chunk1 = "data: "
                         "{\"id\":\"ds-stream-1\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_"
                         "content\":\"Step 1: start thinking...\"}}]}\n\n";
    const char* chunk2 =
        "data: {\"id\":\"ds-stream-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Final "
        "conclusion.\"}}]}\n\n";
    const char* chunk3 = "data: "
                         "{\"id\":\"ds-stream-1\",\"choices\":[],\"usage\":{\"prompt_tokens\":250,"
                         "\"completion_tokens\":75,\"prompt_cache_hit_tokens\":180}}\n\n"
                         "data: [DONE]\n\n";

    TEST_ASSERT(g_provider_openai.stream_bridge_feed(b, chunk1, strlen(chunk1)) == 0,
                "feed chunk 1");
    TEST_ASSERT(g_provider_openai.stream_bridge_feed(b, chunk2, strlen(chunk2)) == 0,
                "feed chunk 2");
    TEST_ASSERT(g_provider_openai.stream_bridge_feed(b, chunk3, strlen(chunk3)) == 0,
                "feed chunk 3");

    TEST_ASSERT(g_provider_openai.stream_bridge_finish(b) == 0, "finish ok");

    long ptok = 0, ctok = 0, cached_tok = 0;
    g_provider_openai.stream_bridge_get_tokens(b, &ptok, &ctok, &cached_tok);

    TEST_ASSERT(ptok == 250, "stream ptok 250, got %ld", ptok);
    TEST_ASSERT(ctok == 75, "stream ctok 75, got %ld", ctok);
    TEST_ASSERT(cached_tok == 180, "stream cached tokens 180, got %ld", cached_tok);

    TEST_ASSERT(strstr(sink.written, "Step 1: start thinking...") != NULL,
                "reasoning_content preserved in stream output");
    TEST_ASSERT(strstr(sink.written, "Final conclusion.") != NULL,
                "content preserved in stream output");

    g_provider_openai.stream_bridge_free(b);
}
