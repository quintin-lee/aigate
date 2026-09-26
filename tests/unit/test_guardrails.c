/** @file test_guardrails.c
 *  @brief Unit tests for Aho-Corasick keyword filtering and guardrails.
 */
#include "run_tests.h"
#include "guardrails.h"
#include <string.h>

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
