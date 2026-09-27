/** @file test_guardrails.c
 *  @brief Unit tests for Aho-Corasick keyword filtering and guardrails.
 */
#include "run_tests.h"
#include "guardrails.h"
#include <string.h>
#include <stdlib.h>

TEST_CASE(test_guardrails_ac_basic)
{
    ac_trie_t* trie = ac_trie_create();
    TEST_ASSERT(trie != NULL, "trie create");

    TEST_ASSERT(ac_trie_insert(trie, "badword") == 0, "insert badword");
    TEST_ASSERT(ac_trie_insert(trie, "danger") == 0, "insert danger");
    TEST_ASSERT(ac_trie_insert(trie, "attack") == 0, "insert attack");
    TEST_ASSERT(ac_trie_build_failure_links(trie) == 0, "build failure links");

    /* 1. Normal clean text */
    const char* clean = "This is a clean and completely harmless prompt.";
    const char* match = ac_trie_search(trie, clean, strlen(clean));
    TEST_ASSERT(match == NULL, "clean text should not match");

    /* 2. Direct match */
    const char* dirty1 = "Beware of the badword in this sentence.";
    match = ac_trie_search(trie, dirty1, strlen(dirty1));
    TEST_ASSERT(match != NULL, "should find match for badword");
    TEST_ASSERT(strcmp(match, "badword") == 0, "match is badword");

    /* 3. Substring match inside words */
    const char* dirty2 = "The system is under attack right now.";
    match = ac_trie_search(trie, dirty2, strlen(dirty2));
    TEST_ASSERT(match != NULL, "should find match for attack");
    TEST_ASSERT(strcmp(match, "attack") == 0, "match is attack");

    /* 4. Match at start and end */
    const char* start_match = "danger zone ahead";
    match = ac_trie_search(trie, start_match, strlen(start_match));
    TEST_ASSERT(match != NULL && strcmp(match, "danger") == 0, "match at start");

    const char* end_match = "entering danger";
    match = ac_trie_search(trie, end_match, strlen(end_match));
    TEST_ASSERT(match != NULL && strcmp(match, "danger") == 0, "match at end");

    ac_trie_destroy(trie);
}

TEST_CASE(test_guardrails_ac_overlapping)
{
    ac_trie_t* trie = ac_trie_create();
    TEST_ASSERT(trie != NULL, "trie create");

    /* Classic Aho-Corasick overlapping test set */
    TEST_ASSERT(ac_trie_insert(trie, "he") == 0, "insert he");
    TEST_ASSERT(ac_trie_insert(trie, "she") == 0, "insert she");
    TEST_ASSERT(ac_trie_insert(trie, "his") == 0, "insert his");
    TEST_ASSERT(ac_trie_insert(trie, "hers") == 0, "insert hers");
    TEST_ASSERT(ac_trie_build_failure_links(trie) == 0, "build links");

    const char* t1 = "ushers";
    const char* m1 = ac_trie_search(trie, t1, strlen(t1));
    TEST_ASSERT(m1 != NULL, "ushers has matches");
    /* Could match 'she', 'he', or 'hers' depending on match propagation order */
    TEST_ASSERT(strcmp(m1, "she") == 0 || strcmp(m1, "he") == 0 || strcmp(m1, "hers") == 0,
                "valid overlapping match");

    const char* t2 = "ahish";
    const char* m2 = ac_trie_search(trie, t2, strlen(t2));
    TEST_ASSERT(m2 != NULL, "ahish has matches");
    TEST_ASSERT(strcmp(m2, "his") == 0, "matched his");

    ac_trie_destroy(trie);
}

TEST_CASE(test_guardrails_ac_edge_cases)
{
    ac_trie_t* trie = ac_trie_create();
    TEST_ASSERT(trie != NULL, "trie create");

    TEST_ASSERT(ac_trie_insert(trie, "x") == 0, "single char");
    TEST_ASSERT(ac_trie_insert(trie, "longkeywordpatternthatshouldnotfail") == 0, "long pattern");
    TEST_ASSERT(ac_trie_build_failure_links(trie) == 0, "build links");

    /* NULL and empty string */
    TEST_ASSERT(ac_trie_search(trie, NULL, 0) == NULL, "NULL search");
    TEST_ASSERT(ac_trie_search(trie, "", 0) == NULL, "empty search");

    /* Single char match */
    const char* m = ac_trie_search(trie, "abcxdef", 7);
    TEST_ASSERT(m != NULL && strcmp(m, "x") == 0, "matched single char");

    ac_trie_destroy(trie);
}

TEST_CASE(test_guardrails_pii_masking)
{
    guardrails_ctx_t* ctx = guardrails_create();
    TEST_ASSERT(ctx != NULL, "guardrails_create");

    int changed = 0;

    /* 1. Phone number masking */
    const char* t_phone = "我的电话是13812345678";
    char* res = guardrails_mask_pii_text(ctx, t_phone, strlen(t_phone), &changed);
    TEST_ASSERT(changed == 1, "phone changed");
    TEST_ASSERT(res != NULL, "phone res not null");
    TEST_ASSERT(strcmp(res, "我的电话是[PHONE]") == 0, "phone masked");
    free(res);

    /* 2. ID card masking */
    const char* t_id = "身份证110101199003072345号";
    changed = 0;
    res = guardrails_mask_pii_text(ctx, t_id, strlen(t_id), &changed);
    TEST_ASSERT(changed == 1, "id changed");
    TEST_ASSERT(res != NULL, "id res not null");
    TEST_ASSERT(strcmp(res, "身份证[ID_CARD]号") == 0, "id card masked");
    free(res);

    /* 3. Email masking */
    const char* t_email = "联系alice@example.com处理";
    changed = 0;
    res = guardrails_mask_pii_text(ctx, t_email, strlen(t_email), &changed);
    TEST_ASSERT(changed == 1, "email changed");
    TEST_ASSERT(res != NULL, "email res not null");
    TEST_ASSERT(strcmp(res, "联系[EMAIL]处理") == 0, "email masked");
    free(res);

    /* 4. API Key masking */
    const char* t_key = "API Key 是 sk-abc12345678901234567890";
    changed = 0;
    res = guardrails_mask_pii_text(ctx, t_key, strlen(t_key), &changed);
    TEST_ASSERT(changed == 1, "key changed");
    TEST_ASSERT(res != NULL, "key res not null");
    TEST_ASSERT(strcmp(res, "API Key 是 [API_KEY]") == 0, "api key masked");
    free(res);

    /* 5. Combined PII */
    const char* t_combo = "用户13800000000的邮箱是bob@corp.cn，密钥ghp_12345678901234567890";
    changed = 0;
    res = guardrails_mask_pii_text(ctx, t_combo, strlen(t_combo), &changed);
    TEST_ASSERT(changed == 1, "combo changed");
    TEST_ASSERT(res != NULL, "combo res not null");
    TEST_ASSERT(strcmp(res, "用户[PHONE]的邮箱是[EMAIL]，密钥[API_KEY]") == 0, "combo masked");
    free(res);

    /* 6. Clean text unchanged */
    const char* t_clean = "没有任何敏感信息的一句话";
    changed = 0;
    res = guardrails_mask_pii_text(ctx, t_clean, strlen(t_clean), &changed);
    TEST_ASSERT(changed == 0, "clean unchanged");
    TEST_ASSERT(res == NULL, "clean returns NULL");

    guardrails_destroy(ctx);
}

TEST_CASE(test_guardrails_inbound_json_inspection)
{
    guardrails_ctx_t* ctx = guardrails_create();
    TEST_ASSERT(ctx != NULL, "guardrails_create");

    /* Load rules: 1 block rule, 1 exempt rule */
    guardrail_rule_t rules[2];
    memset(rules, 0, sizeof rules);
    strcpy(rules[0].rule_type, "keyword");
    strcpy(rules[0].pattern, "drop database");
    strcpy(rules[0].action, "block");
    rules[0].enabled = 1;

    strcpy(rules[1].rule_type, "exempt");
    strcpy(rules[1].pattern, "drop database tutorial");
    strcpy(rules[1].action, "exempt");
    rules[1].enabled = 1;

    TEST_ASSERT(guardrails_load_rules(ctx, rules, 2) == 0, "load rules");

    /* 1. Inbound JSON with PII */
    const char* json_pii =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"我的电话是13912345678\"}]}";
    char*  sanitized = NULL;
    size_t san_len = 0;
    char   blocked_kw[64] = {0};

    guardrails_action_t act = guardrails_inspect_inbound(
        ctx, json_pii, strlen(json_pii), &sanitized, &san_len, blocked_kw, sizeof blocked_kw);
    TEST_ASSERT(act == GUARDRAILS_MASKED, "PII in JSON masked");
    TEST_ASSERT(sanitized != NULL && san_len > 0, "sanitized body non-empty");
    TEST_ASSERT(strstr(sanitized, "[PHONE]") != NULL, "contains [PHONE]");
    TEST_ASSERT(strstr(sanitized, "13912345678") == NULL, "original phone removed");
    free(sanitized);

    /* 2. Inbound JSON with forbidden keyword */
    const char* json_blocked =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"please drop database "
        "now\"}]}";
    sanitized = NULL;
    san_len = 0;
    act = guardrails_inspect_inbound(ctx,
                                     json_blocked,
                                     strlen(json_blocked),
                                     &sanitized,
                                     &san_len,
                                     blocked_kw,
                                     sizeof blocked_kw);
    TEST_ASSERT(act == GUARDRAILS_BLOCKED, "keyword blocked");
    TEST_ASSERT(strcmp(blocked_kw, "drop database") == 0, "blocked keyword reported");
    TEST_ASSERT(sanitized == NULL, "no sanitized body when blocked");

    /* 3. Inbound JSON with keyword but exempted */
    const char* json_exempt =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"read drop database "
        "tutorial\"}]}";
    sanitized = NULL;
    san_len = 0;
    act = guardrails_inspect_inbound(
        ctx, json_exempt, strlen(json_exempt), &sanitized, &san_len, blocked_kw, sizeof blocked_kw);
    TEST_ASSERT(act == GUARDRAILS_PASS, "exempted keyword passes");
    TEST_ASSERT(sanitized == NULL, "no sanitized body when clean");

    guardrails_destroy(ctx);
}

