#include "run_tests.h"
#include "policy/prompt_template.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

static void
test_prompt_expand_vars(void)
{
    const char* tmpl = "Hello ${model} via ${key_name} on ${date}! Custom: ${unknown}";
    char*       out = prompt_template_expand_vars(tmpl, "gpt-4o", "test-key");
    TEST_ASSERT(out != NULL, "expand_vars returned non-null");
    TEST_ASSERT(strstr(out, "Hello gpt-4o via test-key on 202") != NULL,
                "substituted model, key, and date");
    TEST_ASSERT(strstr(out, "Custom: ${unknown}") != NULL, "preserved unknown placeholder");
    free(out);
}

static void
test_prompt_apply_new_system(void)
{
    const char* raw_json =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    json_t* jbody = json_loads(raw_json, 0, NULL);
    TEST_ASSERT(jbody != NULL, "json_loads ok");

    prompt_template_t tmpl = {
        .system_template = "System rule for ${model}",
        .mode = PROMPT_MODE_PREPEND,
        .prefix_user_prompt = NULL,
        .suffix_user_prompt = NULL,
    };

    char*  mod_json = NULL;
    size_t mod_len = 0;
    int    rc = prompt_template_apply(&tmpl, "gpt-4o", "k1", jbody, &mod_json, &mod_len);
    TEST_ASSERT(rc == 0, "apply succeeded");
    TEST_ASSERT(mod_json != NULL, "modified json produced");

    json_t* res_json = json_loads(mod_json, 0, NULL);
    json_t* msgs = json_object_get(res_json, "messages");
    TEST_ASSERT(json_array_size(msgs) == 2, "messages has 2 elements");

    json_t*     msg0 = json_array_get(msgs, 0);
    const char* r0 = json_string_value(json_object_get(msg0, "role"));
    const char* c0 = json_string_value(json_object_get(msg0, "content"));
    TEST_ASSERT(strcmp(r0, "system") == 0, "first message is system");
    TEST_ASSERT(strcmp(c0, "System rule for gpt-4o") == 0, "content matched expected");

    json_decref(res_json);
    free(mod_json);
    json_decref(jbody);
}

static void
test_prompt_apply_prepend_append_override(void)
{
    const char* raw_json =
        "{\"model\":\"gpt-4o\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"Original sys\"},"
        "{\"role\":\"user\",\"content\":\"User msg\"}]}";

    /* 1. Prepend */
    {
        json_t*           jbody = json_loads(raw_json, 0, NULL);
        prompt_template_t tmpl = {.system_template = "Injected", .mode = PROMPT_MODE_PREPEND};
        char*             mod_json = NULL;
        TEST_ASSERT(prompt_template_apply(&tmpl, "m", "k", jbody, &mod_json, NULL) == 0,
                    "prepend ok");
        json_t*     res = json_loads(mod_json, 0, NULL);
        const char* c = json_string_value(
            json_object_get(json_array_get(json_object_get(res, "messages"), 0), "content"));
        TEST_ASSERT(strcmp(c, "Injected\n\nOriginal sys") == 0, "prepend content matched: %s", c);
        json_decref(res);
        free(mod_json);
        json_decref(jbody);
    }

    /* 2. Append */
    {
        json_t*           jbody = json_loads(raw_json, 0, NULL);
        prompt_template_t tmpl = {.system_template = "Injected", .mode = PROMPT_MODE_APPEND};
        char*             mod_json = NULL;
        TEST_ASSERT(prompt_template_apply(&tmpl, "m", "k", jbody, &mod_json, NULL) == 0,
                    "append ok");
        json_t*     res = json_loads(mod_json, 0, NULL);
        const char* c = json_string_value(
            json_object_get(json_array_get(json_object_get(res, "messages"), 0), "content"));
        TEST_ASSERT(strcmp(c, "Original sys\n\nInjected") == 0, "append content matched: %s", c);
        json_decref(res);
        free(mod_json);
        json_decref(jbody);
    }

    /* 3. Override */
    {
        json_t*           jbody = json_loads(raw_json, 0, NULL);
        prompt_template_t tmpl = {.system_template = "Injected", .mode = PROMPT_MODE_OVERRIDE};
        char*             mod_json = NULL;
        TEST_ASSERT(prompt_template_apply(&tmpl, "m", "k", jbody, &mod_json, NULL) == 0,
                    "override ok");
        json_t*     res = json_loads(mod_json, 0, NULL);
        const char* c = json_string_value(
            json_object_get(json_array_get(json_object_get(res, "messages"), 0), "content"));
        TEST_ASSERT(strcmp(c, "Injected") == 0, "override content matched: %s", c);
        json_decref(res);
        free(mod_json);
        json_decref(jbody);
    }
}

static void
test_prompt_apply_user_prefix_suffix(void)
{
    const char* raw_json =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"Question\"}]}";
    json_t* jbody = json_loads(raw_json, 0, NULL);

    prompt_template_t tmpl = {
        .system_template = "System rule",
        .mode = PROMPT_MODE_PREPEND,
        .prefix_user_prompt = "[Prefix] ",
        .suffix_user_prompt = " [Suffix]",
    };

    char* mod_json = NULL;
    TEST_ASSERT(prompt_template_apply(&tmpl, "m", "k", jbody, &mod_json, NULL) == 0, "apply ok");
    json_t*     res = json_loads(mod_json, 0, NULL);
    json_t*     msgs = json_object_get(res, "messages");
    const char* u_content = json_string_value(json_object_get(json_array_get(msgs, 1), "content"));
    TEST_ASSERT(strcmp(u_content, "[Prefix] Question [Suffix]") == 0,
                "user content decorated: %s",
                u_content);

    json_decref(res);
    free(mod_json);
    json_decref(jbody);
}

void
test_prompt_template_suite(void)
{
    TEST_CASE(test_prompt_expand_vars);
    TEST_CASE(test_prompt_apply_new_system);
    TEST_CASE(test_prompt_apply_prepend_append_override);
    TEST_CASE(test_prompt_apply_user_prefix_suffix);
}
