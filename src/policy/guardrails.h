/** @file guardrails.h
 *  @brief Content moderation & guardrails: Aho-Corasick multi-pattern keyword matching
 *         and inbound PII masking pipeline.
 */

/**
 * @defgroup group_policy Policy layer
 * @brief Policy: auth, budget, circuit breaking, guardrails, rate limiting, response cache.
 */
#ifndef AIGATE_GUARDRAILS_H
#define AIGATE_GUARDRAILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "pg_store.h"

/** @brief Inbound check result: pass/masked pass/block. */
typedef enum {
    GUARDRAILS_PASS = 0,   /* Content clean, no masking or blocking */
    GUARDRAILS_MASKED = 1, /* PII found and masked */
    GUARDRAILS_BLOCKED = 2 /* Forbidden keyword found, request blocked */
} guardrails_action_t;

/* --- Aho-Corasick Pattern Matching Trie --- */

/** @brief AC automaton node: transition table/failure link/hit keyword. */
/** @brief AC automaton node: transition table/failure link/hit keyword. */
typedef struct ac_node {
    int   next[256];       /**< Transition on byte 0..255; -1 = uninitialized */
    int   fail;            /**< Failure link index */
    char* matched_keyword; /**< Keyword ending at this node (or NULL) */
} ac_node_t;

/** @brief AC automaton: node pool + used count + capacity. */
typedef struct ac_trie {
    ac_node_t* nodes;      /**< Node pool (index 0 is the root). */
    size_t     node_count; /**< Used node count. */
    size_t     node_cap;   /**< Node pool capacity. */
} ac_trie_t;

/** @brief Create an AC automaton (empty trie, root node only).
 *  @return New instance; NULL on OOM. */
ac_trie_t* ac_trie_create(void);
/** @brief Free the trie and all node keywords (NULL-safe). */
void ac_trie_destroy(ac_trie_t* trie);
/** @brief Insert one keyword (empty string/NULL rejected).
 *  @return 0 on success; -1 on bad arguments or OOM. */
int ac_trie_insert(ac_trie_t* trie, const char* keyword);
/** @brief BFS-build failure links and complete the transition table (call once after insert, before search).
 *  @return 0 on success (empty trie included); -1 on OOM. */
int ac_trie_build_failure_links(ac_trie_t* trie);
/** @brief Search text for the first hit keyword.
 *  @return Hit keyword (borrowed pointer, do not free); NULL on no hit/empty trie. */
const char* ac_trie_search(const ac_trie_t* trie, const char* text, size_t len);

/* --- Full Guardrails Engine --- */

/** @brief Guardrails engine instance (opaque, defined in guardrails.c). */
typedef struct guardrails_ctx guardrails_ctx_t;

/** @brief Create a guardrails engine (compiles PII regexes, empty rule set).
 *  @return New instance; NULL on OOM. */
guardrails_ctx_t* guardrails_create(void);
/** @brief Free the engine (both AC tries and regexes, NULL-safe). */
void guardrails_destroy(guardrails_ctx_t* ctx);
/** @brief Replace the whole rule set (rebuilds the block/exempt tries and builds failure links).
 *  @return 0 on success; -1 on OOM (old rules kept). */
int guardrails_load_rules(guardrails_ctx_t* ctx, const guardrail_rule_t* rules, size_t count);

/** @brief Inbound check: AC blocklist first (hit without exemption yields BLOCKED with the keyword filled back), then PII masking (hit yields MASKED with masked body), else PASS.
 *  @return One of GUARDRAILS_PASS/MASKED/BLOCKED. */
guardrails_action_t guardrails_inspect_inbound(guardrails_ctx_t* ctx,
                                               const char*       raw_body,
                                               size_t            raw_len,
                                               char**            sanitized_body,
                                               size_t*           sanitized_len,
                                               char*             blocked_keyword,
                                               size_t            blocked_keyword_sz);

/** @brief PII masking: API key to [API_KEY], email to [EMAIL], ID card to [ID_CARD], phone to [PHONE].
 *  @param ctx Guardrails engine instance.
 *  @param text Text to mask (NUL termination not required).
 *  @param len Text length.
 *  @param changed Optional, always written with whether a replacement happened.
 *  @return Masked new string (caller frees); NULL on no hit/empty input. */
char* guardrails_mask_pii_text(guardrails_ctx_t* ctx, const char* text, size_t len, int* changed);

#endif /* AIGATE_GUARDRAILS_H */
