/** @file test_provider_anthropic_vision.c
 *  @brief Unit tests for Anthropic vision/multimodal protocol translation.
 */
#include "run_tests.h"
#include "provider_anthropic.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t
make_anthropic_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "claude-3-5-sonnet-20241022");
    snprintf(r.provider, sizeof r.provider, "anthropic");
    snprintf(r.endpoint, sizeof r.endpoint, "http://127.0.0.1:8080");
    snprintf(r.upstream_key, sizeof r.upstream_key, "sk-ant-test");
    return r;
}

/* --------------------------------------------- Test 1: Single Image + Text */
TEST_CASE(test_anthropic_vision_single_image)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Describe this photo\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat.jpg\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(body != NULL, "body not null");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    TEST_ASSERT(msgs && json_is_array(msgs) && json_array_size(msgs) == 1, "1 message");

    json_t* m0 = json_array_get(msgs, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(m0, "role")), "user") == 0, "role=user");

    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_array(content), "content is array");
    TEST_ASSERT(json_array_size(content) == 2, "content has 2 blocks");

    /* Block 0: text */
    json_t* b0 = json_array_get(content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "type")), "text") == 0, "b0 is text");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "text")), "Describe this photo") == 0,
                "b0 text value");

    /* Block 1: image */
    json_t* b1 = json_array_get(content, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "image") == 0,
                "b1 is image");
    json_t* src = json_object_get(b1, "source");
    TEST_ASSERT(src && json_is_object(src), "source object present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src, "type")), "url") == 0,
                "source type=url");
    TEST_ASSERT(
        strcmp(json_string_value(json_object_get(src, "url")), "https://example.com/cat.jpg") == 0,
        "source url value");

    json_decref(out);
    free(body);
}

/* --------------------------------- Test 2: Text-only String Compatibility */
TEST_CASE(test_anthropic_vision_text_only_string)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body = "{\"model\":\"claude-3-5-sonnet-20241022\","
                          "\"messages\":[{\"role\":\"user\",\"content\":\"Just a text prompt\"}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    json_t* m0 = json_array_get(msgs, 0);
    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_string(content), "content is string");
    TEST_ASSERT(strcmp(json_string_value(content), "Just a text prompt") == 0,
                "content string matches");

    json_decref(out);
    free(body);
}

/* ------------------------------------------- Test 3: Multiple Images */
TEST_CASE(test_anthropic_vision_multi_image)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Compare these two photos\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat1.png\"}},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat2.png\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_anthropic_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    json_t* m0 = json_array_get(msgs, 0);
    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_array(content) && json_array_size(content) == 3,
                "content array has 3 blocks");

    json_t* b1 = json_array_get(content, 1);
    json_t* b2 = json_array_get(content, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "image") == 0, "b1 image");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b2, "type")), "image") == 0, "b2 image");

    json_t* src1 = json_object_get(b1, "source");
    json_t* src2 = json_object_get(b2, "source");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src1, "url")),
                       "https://example.com/cat1.png") == 0,
                "url 1");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src2, "url")),
                       "https://example.com/cat2.png") == 0,
                "url 2");

    json_decref(out);
    free(body);
}
