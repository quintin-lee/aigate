/** @file guardrails.h
 *  @brief Content moderation & guardrails: Aho-Corasick multi-pattern keyword matching
 *         and inbound PII masking pipeline.
 */
#ifndef AIGATE_GUARDRAILS_H
#define AIGATE_GUARDRAILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "pg_store.h"

typedef enum {
    GUARDRAILS_PASS = 0,     /* Content clean, no masking or blocking */
    GUARDRAILS_MASKED = 1,   /* PII found and masked */
    GUARDRAILS_BLOCKED = 2   /* Forbidden keyword found, request blocked */
} guardrails_action_t;

/* --- Aho-Corasick Pattern Matching Trie --- */

typedef struct ac_node {
    int   next[256];          /* Transition on byte 0..255; -1 = uninitialized */
    int   fail;               /* Failure link index */
    char* matched_keyword;    /* Keyword ending at this node (or NULL) */
} ac_node_t;

typedef struct ac_trie {
    ac_node_t* nodes;
    size_t     node_count;
    size_t     node_cap;
} ac_trie_t;

ac_trie_t*  ac_trie_create(void);
void        ac_trie_destroy(ac_trie_t* trie);
int         ac_trie_insert(ac_trie_t* trie, const char* keyword);
int         ac_trie_build_failure_links(ac_trie_t* trie);
const char* ac_trie_search(const ac_trie_t* trie, const char* text, size_t len);

/* --- Full Guardrails Engine --- */

typedef struct guardrails_ctx guardrails_ctx_t;

guardrails_ctx_t* guardrails_create(void);
void              guardrails_destroy(guardrails_ctx_t* ctx);
int               guardrails_load_rules(guardrails_ctx_t* ctx, const guardrail_rule_t* rules, size_t count);

guardrails_action_t guardrails_inspect_inbound(
    guardrails_ctx_t* ctx,
    const char*       raw_body,
    size_t            raw_len,
    char**            sanitized_body,
    size_t*           sanitized_len,
    char*             blocked_keyword,
    size_t            blocked_keyword_sz);

#endif /* AIGATE_GUARDRAILS_H */
