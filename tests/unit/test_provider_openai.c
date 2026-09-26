/** @file test_provider_openai.c
 *  @brief Unit tests for OpenAI provider responses endpoint adapter and usage parsing.
 */
#include "run_tests.h"
#include "pg_store.h"
#include "provider_openai.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_openai_responses_build)
{
    model_rec_t route = {
        .name = "gpt-4o",
        .provider = "openai",
        .endpoint = "https://api.openai.com/v1",
        .upstream_key = "sk-test-secret-12345",
    };
    char url[512];
    const char* extra_headers[4][2];
    int n_extra = 0;
    char* body = NULL;
    size_t body_len = 0;

    const char* in_body = "{\"model\":\"gpt-4o\",\"input\":\"Hello world\"}";
    int rc = provider_openai_build_responses(&route, in_body, url, sizeof(url),
                                            extra_headers, &n_extra, &body, &body_len);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(strcmp(url, "https://api.openai.com/v1/responses") == 0, "url ends with /responses");
    TEST_ASSERT(n_extra == 2, "2 headers");
    TEST_ASSERT(strcmp(extra_headers[0][0], "Authorization") == 0, "auth header");
    TEST_ASSERT(strcmp(extra_headers[0][1], "Bearer sk-test-secret-12345") == 0, "bearer key");
    TEST_ASSERT(strcmp(extra_headers[1][0], "Content-Type") == 0, "content-type");
    TEST_ASSERT(body != NULL && strcmp(body, in_body) == 0, "raw body preserved");
    free(body);
}

TEST_CASE(test_openai_responses_parse_usage_nonstream)
{
    const char* json_resp =
        "{\"id\":\"resp_123\",\"object\":\"response\",\"status\":\"completed\","
        "\"usage\":{\"input_tokens\":12,\"output_tokens\":25,"
        "\"input_tokens_details\":{\"cached_tokens\":4},"
        "\"output_tokens_details\":{\"reasoning_tokens\":7}}}";

    long ptok = 0, ctok = 0, cached = 0, reasoning = 0;
    int rc = provider_openai_parse_responses_usage(json_resp, strlen(json_resp),
                                                   &ptok, &ctok, &cached, &reasoning);
    TEST_ASSERT(rc == 0, "parse ok");
    TEST_ASSERT(ptok == 12, "input_tokens 12");
    TEST_ASSERT(ctok == 25, "output_tokens 25");
    TEST_ASSERT(cached == 4, "cached_tokens 4");
    TEST_ASSERT(reasoning == 7, "reasoning_tokens 7");
}

TEST_CASE(test_openai_responses_parse_usage_stream)
{
    const char* sse_stream =
        "event: response.created\ndata: {\"type\":\"response.created\"}\n\n"
        "event: response.output_text.delta\ndata: {\"delta\":\"Hello\"}\n\n"
        "event: response.completed\ndata: {\"type\":\"response.completed\","
        "\"response\":{\"usage\":{\"input_tokens\":15,\"output_tokens\":40,"
        "\"input_tokens_details\":{\"cached_tokens\":5},"
        "\"output_tokens_details\":{\"reasoning_tokens\":10}}}}\n\n";

    long ptok = 0, ctok = 0, cached = 0, reasoning = 0;
    int rc = provider_openai_parse_responses_usage(sse_stream, strlen(sse_stream),
                                                   &ptok, &ctok, &cached, &reasoning);
    TEST_ASSERT(rc == 0, "stream parse ok");
    TEST_ASSERT(ptok == 15, "stream ptok 15");
    TEST_ASSERT(ctok == 40, "stream ctok 40");
    TEST_ASSERT(cached == 5, "stream cached 5");
    TEST_ASSERT(reasoning == 10, "stream reasoning 10");
}
