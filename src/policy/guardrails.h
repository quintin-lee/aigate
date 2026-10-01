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

/* --- External Webhook Moderation Plugin --- */

/** @brief Webhook rule runtime definition. */
typedef struct guardrail_webhook_rule {
    long id;            /**< Unique rule identifier. */
    char url[512];      /**< External webhook inspection HTTP/HTTPS URL. */
    char secret[256];   /**< HMAC shared secret or bearer token. */
    int  timeout_ms;    /**< HTTP client timeout in milliseconds. */
    char fail_mode[16]; /**< "open" | "closed" */
    char phase[16];     /**< "inbound" | "outbound" | "both" */
} guardrail_webhook_rule_t;

/** @brief Inbound Webhook inspection. */
guardrails_action_t guardrails_inspect_webhook_inbound(guardrails_ctx_t* ctx,
                                                       const char*       model,
                                                       long              key_id,
                                                       const char*       raw_body,
                                                       size_t            raw_len,
                                                       char**            sanitized_body,
                                                       size_t*           sanitized_len,
                                                       char*             block_reason,
                                                       size_t            block_reason_sz);

/** @brief Outbound Webhook inspection on LLM response. */
guardrails_action_t guardrails_inspect_webhook_outbound(guardrails_ctx_t* ctx,
                                                        const char*       model,
                                                        long              key_id,
                                                        const char*       response_body,
                                                        size_t            response_len,
                                                        char**            sanitized_body,
                                                        size_t*           sanitized_len,
                                                        char*             block_reason,
                                                        size_t            block_reason_sz);

/** @brief Single standalone probe helper for Webhook test endpoint. */
int guardrails_webhook_probe(const char* url,
                             const char* secret,
                             int         timeout_ms,
                             char*       out_err,
                             size_t      err_sz,
                             double*     out_latency_ms);

/**
 * @brief Validate bank/credit card number using Luhn algorithm (Mod 10).
 * @param digits Numeric string containing only digits '0'-'9'.
 * @return True if length is 13..19 digits and Luhn checksum is valid; false otherwise.
 */
bool guardrails_validate_luhn(const char* digits);

/**
 * @brief Validate Chinese 18-digit resident ID card using ISO 7064:1983.MOD 11-2.
 * @param id_str 18-character string (17 digits + 1 digit/X).
 * @return True if format matches and check character matches MOD 11-2 remainder; false otherwise.
 */
bool guardrails_validate_id_card_mod11(const char* id_str);

/* --- Advanced PII Masking & De-anonymization --- */

/** @brief Supported PII sensitive entity categories. */
typedef enum {
    PII_TYPE_PHONE = 0,
    PII_TYPE_ID_CARD = 1,
    PII_TYPE_BANK_CARD = 2,
    PII_TYPE_EMAIL = 3,
    PII_TYPE_API_KEY = 4,
    PII_TYPE_IP_ADDRESS = 5,
    PII_TYPE_COUNT = 6
} pii_type_t;

/** @brief Processing actions for detected PII entities. */
typedef enum {
    PII_ACTION_OFF = 0,
    PII_ACTION_ANONYMIZE_RESTORE = 1, /**< Replace with [TAG_N] and restore on response */
    PII_ACTION_MASK_PARTIAL = 2,      /**< Keep prefixes/suffixes and mask middle with '*' */
    PII_ACTION_REDACT_TAG = 3,        /**< One-way replacement with [TAG] */
    PII_ACTION_BLOCK = 4              /**< Block the entire request with HTTP 400 */
} pii_action_t;

/** @brief Configuration rule for a single PII entity. */
typedef struct {
    pii_type_t   type;
    char         name[32]; /**< e.g. "phone", "id_card", "bank_card" */
    char         tag[32];  /**< e.g. "PHONE", "ID_CARD", "BANK_CARD" */
    bool         enabled;
    pii_action_t action;
} pii_rule_t;

/** @brief Global PII configuration table. */
typedef struct {
    pii_rule_t rules[PII_TYPE_COUNT];
} pii_config_t;

#define PII_MAX_SESSION_ENTRIES 64

/** @brief One mapping entry between placeholder token and original sensitive text. */
typedef struct {
    char       placeholder[32]; /**< e.g. "[PHONE_1]" */
    char       original[128];   /**< e.g. "13812345678" */
    pii_type_t type;            /**< Entity type */
} pii_entry_t;

/** @brief Request-bound session mapping table. */
typedef struct {
    pii_entry_t entries[PII_MAX_SESSION_ENTRIES];
    int         count;
} pii_session_map_t;

/** @brief Partial mask phone number (e.g. 138****5678). */
void guardrails_mask_partial_phone(const char* src, char* out, size_t out_sz);
/** @brief Partial mask Chinese ID card (e.g. 110101********2375). */
void guardrails_mask_partial_id_card(const char* src, char* out, size_t out_sz);
/** @brief Partial mask bank/credit card (e.g. 622202******7894). */
void guardrails_mask_partial_bank_card(const char* src, char* out, size_t out_sz);
/** @brief Partial mask email (e.g. a***r@company.com). */
void guardrails_mask_partial_email(const char* src, char* out, size_t out_sz);
/** @brief Partial mask API key (e.g. sk-proj-******3456). */
void guardrails_mask_partial_api_key(const char* src, char* out, size_t out_sz);
/** @brief Partial mask IPv4 address (e.g. 192.168.*.*). */
void guardrails_mask_partial_ip(const char* src, char* out, size_t out_sz);

/**
 * @brief Get existing or create new session placeholder for sensitive text.
 * @param map Session map.
 * @param type Entity type.
 * @param original Sensitive plain text.
 * @return Placeholder string (e.g. "[PHONE_1]"), or NULL on map full.
 */
const char*
pii_session_map_get_or_create(pii_session_map_t* map, pii_type_t type, const char* original);

/**
 * @brief Reverse lookup sensitive plain text by placeholder token.
 * @param map Session map.
 * @param placeholder Token to look up (e.g. "[PHONE_1]").
 * @return Original string, or NULL if not found.
 */
const char* pii_session_map_lookup_token(const pii_session_map_t* map, const char* placeholder);

#endif /* AIGATE_GUARDRAILS_H */
