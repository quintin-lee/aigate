/**
 * @file test_cache_optimizer.c
 * @brief Unit tests for prompt cache prefix alignment and optimizer engine.
 */

#include "policy/cache_optimizer.h"
#include "run_tests.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_cache_optimizer_sort_tools)
{
    const char* json_str = "{"
                           "\"model\":\"gpt-4o\","
                           "\"tools\":["
                           "  {\"type\":\"function\",\"function\":{\"name\":\"weather_lookup\"}},"
                           "  {\"type\":\"function\",\"function\":{\"name\":\"calculator\"}},"
                           "  {\"type\":\"function\",\"function\":{\"name\":\"database_query\"}}"
                           "]"
                           "}";

    json_error_t err;
    json_t*      root = json_loads(json_str, 0, &err);
    TEST_ASSERT(root != NULL, "JSON parse failed");

    bool changed = cache_optimizer_sort_tools(root);
    TEST_ASSERT(changed == true, "Tools should have been reordered");

    json_t* tools = json_object_get(root, "tools");
    TEST_ASSERT(tools != NULL, "tools array missing");

    json_t* t0 = json_array_get(tools, 0);
    json_t* t1 = json_array_get(tools, 1);
    json_t* t2 = json_array_get(tools, 2);

    const char* n0 = json_string_value(json_object_get(json_object_get(t0, "function"), "name"));
    const char* n1 = json_string_value(json_object_get(json_object_get(t1, "function"), "name"));
    const char* n2 = json_string_value(json_object_get(json_object_get(t2, "function"), "name"));

    TEST_ASSERT(n0 && strcmp(n0, "calculator") == 0, "expected calculator");
    TEST_ASSERT(n1 && strcmp(n1, "database_query") == 0, "expected database_query");
    TEST_ASSERT(n2 && strcmp(n2, "weather_lookup") == 0, "expected weather_lookup");

    json_decref(root);
}

TEST_CASE(test_cache_optimizer_normalize_whitespace)
{
    const char* raw = "  Hello   world \t from \n\n\n aigate  ";
    char        out_buf[256];
    size_t      out_len =
        cache_optimizer_normalize_whitespace(raw, strlen(raw), out_buf, sizeof(out_buf));
    TEST_ASSERT(out_len > 0, "Whitespace normalization failed");
    TEST_ASSERT(strstr(out_buf, "Hello world from\n\naigate") != NULL,
                "Normalization formatting unexpected");
}
