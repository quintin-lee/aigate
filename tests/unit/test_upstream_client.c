/** @file test_upstream_client.c
 *  @brief upstream_call against the in-process mock upstream. */
#include "run_tests.h"
#include "upstream_client.h"
#include "mock_upstream.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_upstream_200_roundtrip)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    char url[256];
    snprintf(url, sizeof url, "%s/chat", mock_upstream_base(mu));

    int    status = 0;
    char*  body = NULL;
    size_t blen = 0;
    int    rc = upstream_call(
        url, "sk-mock", "{\"model\":\"gpt-4o\",\"messages\":[]}", 0, 5000, &status, &body, &blen);
    TEST_ASSERT(rc == 0, "transport ok, rc=%d", rc);
    TEST_ASSERT(status == 200, "status 200, got %d", status);
    TEST_ASSERT(body != NULL && blen > 0, "body present");

    json_t* j = json_loads(body, 0, NULL);
    TEST_ASSERT(j != NULL, "body is JSON: %s", body ? body : "(null)");
    json_t* usage = json_object_get(j, "usage");
    TEST_ASSERT(usage != NULL, "usage present");
    TEST_ASSERT(json_integer_value(json_object_get(usage, "prompt_tokens")) == 7,
                "prompt_tokens 7");
    TEST_ASSERT(json_integer_value(json_object_get(usage, "completion_tokens")) == 11,
                "completion_tokens 11");
    json_decref(j);
    free(body);

    /* mock recorded the request with its body */
    TEST_ASSERT(mock_upstream_request_count(mu) == 1, "one request served");
    TEST_ASSERT(strcmp(mock_upstream_last_path(mu), "/chat") == 0, "path /chat");
    TEST_ASSERT(strstr(mock_upstream_last_body(mu), "gpt-4o") != NULL, "body recorded");
    mock_upstream_stop(mu);
}

TEST_CASE(test_upstream_500_passthrough)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    char url[256];
    snprintf(url, sizeof url, "%s/fail", mock_upstream_base(mu));

    int    status = 0;
    char*  body = NULL;
    size_t blen = 0;
    int    rc = upstream_call(url, "", "{}", 0, 5000, &status, &body, &blen);
    TEST_ASSERT(rc == 0, "transport ok even on 5xx, rc=%d", rc);
    TEST_ASSERT(status == 500, "status 500, got %d", status);
    free(body);
    mock_upstream_stop(mu);
}

TEST_CASE(test_upstream_fail_all_toggle)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    char url[256];
    snprintf(url, sizeof url, "%s/chat/completions", mock_upstream_base(mu));

    int    status = 0;
    char*  body = NULL;
    size_t blen = 0;
    int    rc = upstream_call(url, "", "{}", 0, 5000, &status, &body, &blen);
    TEST_ASSERT(rc == 0 && status == 200, "baseline 200, got %d", status);
    free(body);

    mock_upstream_fail_all(mu, 1);
    rc = upstream_call(url, "", "{}", 0, 5000, &status, &body, &blen);
    TEST_ASSERT(rc == 0 && status == 500, "toggled 500, got %d", status);
    free(body);
    mock_upstream_stop(mu);
}

TEST_CASE(test_upstream_timeout)
{
    mock_upstream_t* mu = mock_upstream_start();
    TEST_ASSERT(mu != NULL, "mock started");
    char url[256];
    snprintf(url, sizeof url, "%s/slow", mock_upstream_base(mu));

    int    status = 0;
    char*  body = NULL;
    size_t blen = 0;
    int    rc = upstream_call(url, "", "{}", 0, 200, &status, &body, &blen);
    TEST_ASSERT(rc == -110, "timeout → -110, got %d", rc);
    mock_upstream_stop(mu);
}
