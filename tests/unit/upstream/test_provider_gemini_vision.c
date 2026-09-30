/** @file test_provider_gemini_vision.c
 *  @brief Unit tests for Gemini vision/multimodal protocol translation.
 */
#include "run_tests.h"
#include "provider_gemini.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t
make_gemini_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "gemini-1.5-pro");
    snprintf(r.provider, sizeof r.provider, "gemini");
    snprintf(r.endpoint, sizeof r.endpoint, "https://generativelanguage.googleapis.com");
    snprintf(r.upstream_key, sizeof r.upstream_key, "AIzaSyTest");
    return r;
}

/* ------------------------------------- Test 1: Single Image with Extension */
TEST_CASE(test_gemini_vision_single_image)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Describe this photo\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat.png\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(body != NULL, "body not null");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    TEST_ASSERT(contents && json_is_array(contents) && json_array_size(contents) == 1, "1 content");

    json_t* entry = json_array_get(contents, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(entry, "role")), "user") == 0,
                "role=user");

    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_is_array(parts) && json_array_size(parts) == 2, "2 parts");

    /* Part 0: text */
    json_t* p0 = json_array_get(parts, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "text")), "Describe this photo") == 0,
                "text match");

    /* Part 1: fileData */
    json_t* p1 = json_array_get(parts, 1);
    json_t* fd = json_object_get(p1, "fileData");
    TEST_ASSERT(fd && json_is_object(fd), "fileData present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "fileUri")),
                       "https://example.com/cat.png") == 0,
                "fileUri match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "mimeType")), "image/png") == 0,
                "mimeType=image/png");

    json_decref(out);
    free(body);
}

/* --------------------------------------- Test 2: Fallback MIME Type */
TEST_CASE(test_gemini_vision_unknown_mime_fallback)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/media?file=123\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    json_t* entry = json_array_get(contents, 0);
    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_array_size(parts) == 1, "1 part");

    json_t* fd = json_object_get(json_array_get(parts, 0), "fileData");
    TEST_ASSERT(fd != NULL, "fileData present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "fileUri")),
                       "https://example.com/media?file=123") == 0,
                "fileUri match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "mimeType")), "image/jpeg") == 0,
                "mimeType fallback to image/jpeg");

    json_decref(out);
    free(body);
}

/* --------------------------------- Test 3: Text-only String Compatibility */
TEST_CASE(test_gemini_vision_text_only_string)
{
    model_rec_t route = make_gemini_route();
    const char* in_body = "{\"model\":\"gemini-1.5-pro\","
                          "\"messages\":[{\"role\":\"user\",\"content\":\"Hello Gemini\"}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int rc = provider_gemini_build(&route, in_body, url, sizeof url, hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    json_t* entry = json_array_get(contents, 0);
    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_array_size(parts) == 1, "1 part");
    json_t* p0 = json_array_get(parts, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "text")), "Hello Gemini") == 0,
                "text match");

    json_decref(out);
    free(body);
}
