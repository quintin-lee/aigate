/** @file test_provider_gemini.c
 *  @brief Unit tests for Google Gemini request/response translation (Plan 3, Task 3).
 */
#include "run_tests.h"
#include "provider_adapter.h"
#include "provider_gemini.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_gemini_build_system_and_contents)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "gemini-1.5-pro");
    snprintf(route.provider, sizeof route.provider, "gemini");
    snprintf(route.endpoint, sizeof route.endpoint, "https://generativelanguage.googleapis.com");
    snprintf(route.upstream_key, sizeof route.upstream_key, "AIzaSyTestKey");

    const char* in_req =
        "{\"model\":\"gemini-1.5-pro\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"You are an assistant.\"},"
        "{\"role\":\"user\",\"content\":\"Hi Gemini\"},"
        "{\"role\":\"assistant\",\"content\":\"Hello there!\"},"
        "{\"role\":\"user\",\"content\":\"How are you?\"}"
        "]}";

    char url[512];
    const char* hdrs[4][2];
    int n_hdrs = 0;
    char* body = NULL;
    size_t body_len = 0;

    int rc = provider_gemini_build(&route, in_req, url, sizeof url, hdrs, &n_hdrs, &body, &body_len);
    TEST_ASSERT(rc == 0, "gemini build ok");
    TEST_ASSERT(strstr(url, "/v1beta/models/gemini-1.5-pro:generateContent") != NULL,
                "url contains generateContent, got %s", url);
    TEST_ASSERT(n_hdrs == 1, "1 extra header");
    TEST_ASSERT(strcmp(hdrs[0][0], "x-goog-api-key") == 0, "x-goog-api-key header");
    TEST_ASSERT(strcmp(hdrs[0][1], "AIzaSyTestKey") == 0, "key value matches");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "parsed output json");
    if (out != NULL) {
        json_t* sys = json_object_get(out, "systemInstruction");
        TEST_ASSERT(sys != NULL, "systemInstruction exists");
        if (sys != NULL) {
            json_t* parts = json_object_get(sys, "parts");
            TEST_ASSERT(parts != NULL && json_is_array(parts), "parts is array");
            json_t* p0 = json_array_get(parts, 0);
            json_t* jt = json_object_get(p0, "text");
            TEST_ASSERT(jt && strcmp(json_string_value(jt), "You are an assistant.") == 0,
                        "system instruction text");
        }

        json_t* contents = json_object_get(out, "contents");
        TEST_ASSERT(contents && json_is_array(contents), "contents is array");
        if (contents && json_is_array(contents)) {
            TEST_ASSERT(json_array_size(contents) == 3, "3 user/model messages");
            json_t* m0 = json_array_get(contents, 0);
            TEST_ASSERT(strcmp(json_string_value(json_object_get(m0, "role")), "user") == 0, "role 0 user");
            json_t* m1 = json_array_get(contents, 1);
            TEST_ASSERT(strcmp(json_string_value(json_object_get(m1, "role")), "model") == 0, "role 1 model");
            json_t* m2 = json_array_get(contents, 2);
            TEST_ASSERT(strcmp(json_string_value(json_object_get(m2, "role")), "user") == 0, "role 2 user");
        }
        json_decref(out);
    }
    free(body);
}

TEST_CASE(test_gemini_build_generation_config)
{
    model_rec_t route;
    memset(&route, 0, sizeof route);
    snprintf(route.name, sizeof route.name, "gemini-1.5-flash");
    snprintf(route.provider, sizeof route.provider, "google");
    snprintf(route.upstream_key, sizeof route.upstream_key, "key123");

    const char* in_req =
        "{\"model\":\"gemini-1.5-flash\",\"temperature\":0.7,\"max_tokens\":256,\"top_p\":0.9,\"stop\":[\"END\",\"STOP\"],"
        "\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}";

    char url[512];
    const char* hdrs[4][2];
    int n_hdrs = 0;
    char* body = NULL;
    size_t body_len = 0;

    int rc = provider_gemini_build(&route, in_req, url, sizeof url, hdrs, &n_hdrs, &body, &body_len);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "parsed output");
    if (out != NULL) {
        json_t* gc = json_object_get(out, "generationConfig");
        TEST_ASSERT(gc && json_is_object(gc), "generationConfig object");
        if (gc) {
            json_t* jt = json_object_get(gc, "temperature");
            TEST_ASSERT(jt && json_number_value(jt) > 0.69 && json_number_value(jt) < 0.71, "temperature 0.7");
            json_t* jm = json_object_get(gc, "maxOutputTokens");
            TEST_ASSERT(jm && json_integer_value(jm) == 256, "maxOutputTokens 256");
            json_t* jtop = json_object_get(gc, "topP");
            TEST_ASSERT(jtop && json_number_value(jtop) > 0.89 && json_number_value(jtop) < 0.91, "topP 0.9");
            json_t* jstop = json_object_get(gc, "stopSequences");
            TEST_ASSERT(jstop && json_is_array(jstop) && json_array_size(jstop) == 2, "2 stop sequences");
        }
        json_decref(out);
    }
    free(body);
}

TEST_CASE(test_gemini_resp_translation)
{
    const char* raw_gemini =
        "{\"candidates\":[{"
        "\"content\":{\"parts\":[{\"text\":\"Gemini says hello!\"}],\"role\":\"model\"},"
        "\"finishReason\":\"STOP\""
        "}],"
        "\"usageMetadata\":{"
        "\"promptTokenCount\":18,"
        "\"candidatesTokenCount\":6,"
        "\"totalTokenCount\":24"
        "}}";

    char* out = NULL;
    size_t out_len = 0;
    long ptok = 0, ctok = 0;

    int rc = provider_gemini_resp_to_openai(raw_gemini, "gemini-1.5-flash", &out, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "translate ok");
    TEST_ASSERT(ptok == 18, "ptok 18, got %ld", ptok);
    TEST_ASSERT(ctok == 6, "ctok 6, got %ld", ctok);
    TEST_ASSERT(out != NULL, "out allocated");

    json_t* root = json_loads(out, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json");
    if (root != NULL) {
        json_t* choices = json_object_get(root, "choices");
        TEST_ASSERT(choices && json_is_array(choices), "choices is array");
        json_t* c0 = json_array_get(choices, 0);
        json_t* msg = json_object_get(c0, "message");
        TEST_ASSERT(msg && json_is_object(msg), "message object");
        json_t* cont = json_object_get(msg, "content");
        TEST_ASSERT(cont && strcmp(json_string_value(cont), "Gemini says hello!") == 0, "content string");
        json_t* fr = json_object_get(c0, "finish_reason");
        TEST_ASSERT(fr && strcmp(json_string_value(fr), "stop") == 0, "finish_reason stop");

        json_t* usg = json_object_get(root, "usage");
        TEST_ASSERT(usg != NULL, "usage object");
        TEST_ASSERT(json_integer_value(json_object_get(usg, "prompt_tokens")) == 18, "prompt_tokens 18");
        TEST_ASSERT(json_integer_value(json_object_get(usg, "completion_tokens")) == 6, "completion_tokens 6");
        json_decref(root);
    }
    free(out);
}

TEST_CASE(test_gemini_resp_error_unwrapping)
{
    const char* raw_err =
        "{\"error\":{\"code\":400,\"message\":\"API key not valid. Please pass a valid API key.\",\"status\":\"INVALID_ARGUMENT\"}}";

    char* out = NULL;
    size_t out_len = 0;
    long ptok = 0, ctok = 0;

    int rc = provider_gemini_resp_to_openai(raw_err, "gemini-1.5-pro", &out, &out_len, &ptok, &ctok);
    TEST_ASSERT(rc == 0, "error unwrapping handled");
    TEST_ASSERT(out != NULL, "out allocated");

    json_t* root = json_loads(out, 0, NULL);
    TEST_ASSERT(root != NULL, "valid json");
    if (root != NULL) {
        json_t* err = json_object_get(root, "error");
        TEST_ASSERT(err != NULL && json_is_object(err), "error object");
        json_t* msg = json_object_get(err, "message");
        TEST_ASSERT(msg && strstr(json_string_value(msg), "API key not valid") != NULL, "message contains error");
        json_t* code = json_object_get(err, "code");
        TEST_ASSERT(code && json_integer_value(code) == 400, "code 400");
        json_decref(root);
    }
    free(out);
}
