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
    char*       res = guardrails_mask_pii_text(ctx, t_phone, strlen(t_phone), &changed);
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
    const char* json_pii = "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":"
                           "\"我的电话是13912345678\"}]}";
    char*       sanitized = NULL;
    size_t      san_len = 0;
    char        blocked_kw[64] = {0};

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

TEST_CASE(test_guardrails_webhook_unit)
{
    /* 1. Standalone probe tests with invalid/unreachable endpoints */
    char   err_msg[256] = {0};
    double latency_ms = 0.0;
    int    prc = guardrails_webhook_probe(NULL, NULL, 50, err_msg, sizeof err_msg, &latency_ms);
    TEST_ASSERT(prc == -1, "probe NULL url -> -1");
    TEST_ASSERT(err_msg[0] != '\0', "probe NULL url error message set");

    prc = guardrails_webhook_probe("", "", 50, err_msg, sizeof err_msg, &latency_ms);
    TEST_ASSERT(prc == -1, "probe empty url -> -1");

    prc = guardrails_webhook_probe(
        "http://127.0.0.1:1/probe", "secret-tok", 50, err_msg, sizeof err_msg, &latency_ms);
    TEST_ASSERT(prc == -1, "probe unreachable url -> -1");
    TEST_ASSERT(err_msg[0] != '\0', "probe unreachable error message populated");

    /* 2. Webhook engine inspection with no webhook rules */
    guardrails_ctx_t* ctx = guardrails_create();
    TEST_ASSERT(ctx != NULL, "guardrails_create");

    char*               sanitized = NULL;
    size_t              san_len = 0;
    char                reason[128] = {0};
    guardrails_action_t act;

    act = guardrails_inspect_webhook_inbound(
        ctx, "gpt-4o", 1, "{\"prompt\":\"hi\"}", 15, &sanitized, &san_len, reason, sizeof reason);
    TEST_ASSERT(act == GUARDRAILS_PASS, "no webhook rules -> inbound pass");

    act = guardrails_inspect_webhook_outbound(
        ctx, "gpt-4o", 1, "{\"choices\":[]}", 14, &sanitized, &san_len, reason, sizeof reason);
    TEST_ASSERT(act == GUARDRAILS_PASS, "no webhook rules -> outbound pass");

    /* 3. Load rules: verify webhook rule does not pollute keyword AC trie */
    guardrail_rule_t r[2];
    memset(r, 0, sizeof r);
    strcpy(r[0].rule_type, "webhook");
    strcpy(r[0].pattern, "http://127.0.0.1:1/moderation");
    strcpy(r[0].action, "block");
    strcpy(r[0].fail_mode, "open");
    strcpy(r[0].phase, "both");
    r[0].timeout_ms = 50;
    r[0].enabled = 1;

    strcpy(r[1].rule_type, "keyword");
    strcpy(r[1].pattern, "bad_payload_keyword");
    strcpy(r[1].action, "block");
    r[1].enabled = 1;

    TEST_ASSERT(guardrails_load_rules(ctx, r, 2) == 0, "load rules with webhook and keyword");

    /* Ensure pattern in webhook rule is NOT matched by L1 keyword inspection */
    const char* txt_webhook = "Check http://127.0.0.1:1/moderation please";
    char        kw_reason[64] = {0};
    act = guardrails_inspect_inbound(
        ctx, txt_webhook, strlen(txt_webhook), &sanitized, &san_len, kw_reason, sizeof kw_reason);
    TEST_ASSERT(act == GUARDRAILS_PASS, "webhook URL not treated as blocked keyword in L1");

    /* 4. Test fail-open on network failure */
    const char* inbound_sample =
        "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    act = guardrails_inspect_webhook_inbound(ctx,
                                             "gpt-4o",
                                             1,
                                             inbound_sample,
                                             strlen(inbound_sample),
                                             &sanitized,
                                             &san_len,
                                             reason,
                                             sizeof reason);
    TEST_ASSERT(act == GUARDRAILS_PASS, "fail-open on network failure returns PASS");
    TEST_ASSERT(sanitized == NULL, "sanitized body NULL on pass");

    /* 5. Test fail-closed on network failure */
    strcpy(r[0].fail_mode, "closed");
    TEST_ASSERT(guardrails_load_rules(ctx, r, 2) == 0, "reload rules with fail-closed");

    act = guardrails_inspect_webhook_inbound(ctx,
                                             "gpt-4o",
                                             1,
                                             inbound_sample,
                                             strlen(inbound_sample),
                                             &sanitized,
                                             &san_len,
                                             reason,
                                             sizeof reason);
    TEST_ASSERT(act == GUARDRAILS_BLOCKED, "fail-closed on network failure returns BLOCKED");
    TEST_ASSERT(strstr(reason, "unavailable") != NULL, "unavailable reason set");

    const char* outbound_sample =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello reply\"}}]}";
    act = guardrails_inspect_webhook_outbound(ctx,
                                              "gpt-4o",
                                              1,
                                              outbound_sample,
                                              strlen(outbound_sample),
                                              &sanitized,
                                              &san_len,
                                              reason,
                                              sizeof reason);
    TEST_ASSERT(act == GUARDRAILS_BLOCKED, "fail-closed outbound network failure returns BLOCKED");

    guardrails_destroy(ctx);
}

TEST_CASE(test_pii_checksum_algorithms)
{
    /* 1. Luhn Mod 10 for Bank / Credit Card */
    /* Valid test cards */
    TEST_ASSERT(guardrails_validate_luhn("4532015112830366") == true,
                "valid luhn Visa (16 digits)");
    TEST_ASSERT(guardrails_validate_luhn("6222021234567894") == true,
                "valid luhn UnionPay (16 digits)");
    TEST_ASSERT(guardrails_validate_luhn("378282246310005") == true, "valid luhn Amex (15 digits)");
    /* Invalid cards / random numbers / wrong lengths */
    TEST_ASSERT(guardrails_validate_luhn("4532015112830367") == false, "invalid luhn check digit");
    TEST_ASSERT(guardrails_validate_luhn("1234567890123456") == false,
                "sequential digits fail luhn");
    TEST_ASSERT(guardrails_validate_luhn("12345") == false, "too short card fails luhn");
    TEST_ASSERT(guardrails_validate_luhn("123456789012345678901") == false,
                "too long card fails luhn");
    TEST_ASSERT(guardrails_validate_luhn(NULL) == false, "null string fails luhn");

    /* 2. ISO 7064:1983.MOD 11-2 for Chinese 18-digit ID Card */
    /* Valid ID cards (Standard checksums) */
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072375") == true,
                "valid id card digit 1");
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072383") == true,
                "valid id card digit 2");
    /* Valid ID with 'X' check digit */
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307002X") == true,
                "valid id card with X");
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307002x") == true,
                "valid id card with lowercase x");
    /* Invalid ID cards */
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072378") == false,
                "wrong checksum digit fails");
    TEST_ASSERT(guardrails_validate_id_card_mod11("123456789012345678") == false,
                "random 18 digits fail MOD 11-2");
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307") == false, "short length fails");
    TEST_ASSERT(guardrails_validate_id_card_mod11(NULL) == false, "null fails");
}

TEST_CASE(test_pii_session_map_and_partial_masking)
{
    /* 1. Partial masking tests */
    char masked[128];

    guardrails_mask_partial_phone("13812345678", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "138****5678") == 0, "phone partial mask 138****5678");

    guardrails_mask_partial_id_card("110101199003072375", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "110101********2375") == 0,
                "id card partial mask 110101********2375");

    guardrails_mask_partial_bank_card("6222021234567894", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "622202******7894") == 0, "bank card partial mask 622202******7894");

    guardrails_mask_partial_email("alice.wonder@company.com", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "a***r@company.com") == 0, "email partial mask a***r@company.com");

    guardrails_mask_partial_api_key("sk-proj-1234567890abcdef123456", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "sk-proj-******3456") == 0,
                "api key partial mask sk-proj-******3456");

    guardrails_mask_partial_ip("192.168.1.100", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "192.168.*.*") == 0, "ip partial mask 192.168.*.*");

    /* 2. Session mapping table operations */
    pii_session_map_t map;
    memset(&map, 0, sizeof map);

    const char* tok1 = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13812345678");
    TEST_ASSERT(tok1 != NULL, "tok1 created");
    TEST_ASSERT(strcmp(tok1, "[PHONE_1]") == 0, "first phone token [PHONE_1]");
    TEST_ASSERT(map.count == 1, "map count is 1");

    /* Same value returns existing token (referential consistency) */
    const char* tok1_dup = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13812345678");
    TEST_ASSERT(strcmp(tok1_dup, "[PHONE_1]") == 0, "duplicate returns same token");
    TEST_ASSERT(map.count == 1, "map count unchanged on duplicate");

    /* Second distinct value creates [PHONE_2] */
    const char* tok2 = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13900001111");
    TEST_ASSERT(strcmp(tok2, "[PHONE_2]") == 0, "second phone token [PHONE_2]");
    TEST_ASSERT(map.count == 2, "map count is 2");

    /* Reverse lookup */
    const char* orig1 = pii_session_map_lookup_token(&map, "[PHONE_1]");
    TEST_ASSERT(orig1 != NULL && strcmp(orig1, "13812345678") == 0,
                "lookup [PHONE_1] returns original");
    const char* orig2 = pii_session_map_lookup_token(&map, "[PHONE_2]");
    TEST_ASSERT(orig2 != NULL && strcmp(orig2, "13900001111") == 0,
                "lookup [PHONE_2] returns original");
    TEST_ASSERT(pii_session_map_lookup_token(&map, "[PHONE_3]") == NULL,
                "lookup missing token returns NULL");
}
