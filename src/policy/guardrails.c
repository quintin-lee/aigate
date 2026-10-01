/** @file guardrails.c
 *  @brief Content moderation & guardrails implementation:
 *         Aho-Corasick multi-pattern trie, POSIX regex PII scanning,
 *         and inbound payload sanitization.
 */
#include "guardrails.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <regex.h>
#include <ctype.h>
#include <jansson.h>
#include <curl/curl.h>
#include "aigate_log.h"

/** @brief Initial AC automaton node capacity (grown on demand). */
#define AC_INIT_CAP 256

/** @brief Maximum concurrent external webhook rules. */
#define MAX_WEBHOOK_RULES 16

ac_trie_t*
ac_trie_create(void)
{
    ac_trie_t* trie = calloc(1, sizeof(*trie));
    if (trie == NULL) {
        return NULL;
    }
    trie->nodes = malloc(sizeof(ac_node_t) * AC_INIT_CAP);
    if (trie->nodes == NULL) {
        free(trie);
        return NULL;
    }
    trie->node_cap = AC_INIT_CAP;
    trie->node_count = 1;

    /* Initialize root node (index 0) */
    for (int i = 0; i < 256; i++) {
        trie->nodes[0].next[i] = -1;
    }
    trie->nodes[0].fail = 0;
    trie->nodes[0].matched_keyword = NULL;

    return trie;
}

void
ac_trie_destroy(ac_trie_t* trie)
{
    if (trie == NULL) {
        return;
    }
    for (size_t i = 0; i < trie->node_count; i++) {
        free(trie->nodes[i].matched_keyword);
    }
    free(trie->nodes);
    free(trie);
}

int
ac_trie_insert(ac_trie_t* trie, const char* keyword)
{
    if (trie == NULL || keyword == NULL || keyword[0] == '\0') {
        return -1;
    }
    int curr = 0;
    /* Step 1: Traverse or branch node for each byte in the UTF-8 keyword */
    for (size_t i = 0; keyword[i] != '\0'; i++) {
        unsigned char c = (unsigned char)keyword[i];
        if (trie->nodes[curr].next[c] == -1) {
            /* Step 2: Dynamic capacity doubling when node table fills */
            if (trie->node_count >= trie->node_cap) {
                size_t     new_cap = trie->node_cap * 2;
                ac_node_t* new_nodes = realloc(trie->nodes, sizeof(ac_node_t) * new_cap);
                if (new_nodes == NULL) {
                    return -1;
                }
                trie->nodes = new_nodes;
                trie->node_cap = new_cap;
            }
            int next_idx = (int)trie->node_count++;
            for (int j = 0; j < 256; j++) {
                trie->nodes[next_idx].next[j] = -1;
            }
            trie->nodes[next_idx].fail = 0;
            trie->nodes[next_idx].matched_keyword = NULL;
            trie->nodes[curr].next[c] = next_idx;
        }
        curr = trie->nodes[curr].next[c];
    }
    /* Step 3: Attach duplicated keyword string to the terminal leaf state */
    if (trie->nodes[curr].matched_keyword == NULL) {
        trie->nodes[curr].matched_keyword = strdup(keyword);
        if (trie->nodes[curr].matched_keyword == NULL) {
            return -1;
        }
    }
    return 0;
}

int
ac_trie_build_failure_links(ac_trie_t* trie)
{
    if (trie == NULL || trie->node_count <= 1) {
        return 0;
    }

    int* queue = malloc(sizeof(int) * trie->node_count);
    if (queue == NULL) {
        return -1;
    }
    size_t q_head = 0, q_tail = 0;

    /* Step 1: Root self-loops & Level-1 seeding.
     * Missing characters on root state (0) loop back to 0. Level-1 children set fail=0
     * and are pushed into the BFS queue. */
    for (int c = 0; c < 256; c++) {
        int next_node = trie->nodes[0].next[c];
        if (next_node != -1) {
            trie->nodes[next_node].fail = 0;
            queue[q_tail++] = next_node;
        } else {
            trie->nodes[0].next[c] = 0;
        }
    }

    /* Step 2: Breadth-First Search (BFS) failure link derivation */
    while (q_head < q_tail) {
        int r = queue[q_head++];
        for (int c = 0; c < 256; c++) {
            int u = trie->nodes[r].next[c];
            if (u != -1) {
                /* Child u exists: compute fail(u) = next[fail(r)][c] */
                int fail_state = trie->nodes[r].fail;
                trie->nodes[u].fail = trie->nodes[fail_state].next[c];

                /* Step 3: Output Link Compression (Keyword Inheritance).
                 * If the failure state is a match, propagate it to node u so matching
                 * requires no runtime traversal of failure ancestors. */
                if (trie->nodes[u].matched_keyword == NULL &&
                    trie->nodes[trie->nodes[u].fail].matched_keyword != NULL) {
                    trie->nodes[u].matched_keyword =
                        strdup(trie->nodes[trie->nodes[u].fail].matched_keyword);
                }

                queue[q_tail++] = u;
            } else {
                /* Step 4: DFA State Compression Invariant.
                 * Redirect missing transition directly to failure state's transition:
                 * next[r][c] = next[fail(r)][c]. Converts the AC tree into a full DFA,
                 * ensuring O(1) single-lookup state transitions per character during search. */
                int fail_state = trie->nodes[r].fail;
                trie->nodes[r].next[c] = trie->nodes[fail_state].next[c];
            }
        }
    }

    free(queue);
    return 0;
}

const char*
ac_trie_search(const ac_trie_t* trie, const char* text, size_t len)
{
    if (trie == NULL || trie->node_count <= 1 || text == NULL || len == 0) {
        return NULL;
    }
    int state = 0;
    /* Step 1: O(|text|) single-pass deterministic DFA search without backtracking loops */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        state = trie->nodes[state].next[c];
        /* Step 2: Immediate keyword match check on current DFA state */
        if (trie->nodes[state].matched_keyword != NULL) {
            return trie->nodes[state].matched_keyword;
        }
    }
    return NULL;
}

/* --- Guardrails Engine Context --- */

/** @brief Guardrails engine instance: rwlock/block+exempt tries/PII regex group/ready flag. */
struct guardrails_ctx {
    pthread_rwlock_t rwlock;            /**< Rule hot-reload rwlock. */
    ac_trie_t*       ac_block;          /**< Blocklist keyword trie. */
    ac_trie_t*       ac_exempt;         /**< Exempt keyword trie. */
    regex_t          re_api_key;        /**< API key regex. */
    regex_t          re_email;          /**< Email regex. */
    regex_t          re_id_card;        /**< ID card number regex. */
    regex_t          re_phone;          /**< Phone number regex. */
    int              regex_ready;       /**< Regex compilation ready flag. */
    guardrail_webhook_rule_t
           webhooks[MAX_WEBHOOK_RULES]; /**< Registered webhook inspection rules. */
    size_t webhook_count;               /**< Number of active webhook rules in table. */
};

guardrails_ctx_t*
guardrails_create(void)
{
    guardrails_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    pthread_rwlock_init(&ctx->rwlock, NULL);
    ctx->ac_block = ac_trie_create();
    ctx->ac_exempt = ac_trie_create();

    /* Compile POSIX regular expressions for PII scanning */
    regcomp(&ctx->re_api_key,
            "sk-[a-zA-Z0-9]{20,}|aig_[a-zA-Z0-9]{20,}|ghp_[a-zA-Z0-9]{20,}",
            REG_EXTENDED);
    regcomp(&ctx->re_email, "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}", REG_EXTENDED);
    regcomp(&ctx->re_id_card,
            "[1-9][0-9]{5}(18|19|20)[0-9]{2}(0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])[0-9]{3}[0-9Xx]",
            REG_EXTENDED);
    regcomp(&ctx->re_phone, "1[3-9][0-9]{9}", REG_EXTENDED);
    ctx->regex_ready = 1;

    return ctx;
}

void
guardrails_destroy(guardrails_ctx_t* ctx)
{
    if (ctx == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&ctx->rwlock);
    ac_trie_destroy(ctx->ac_block);
    ac_trie_destroy(ctx->ac_exempt);
    if (ctx->regex_ready) {
        regfree(&ctx->re_api_key);
        regfree(&ctx->re_email);
        regfree(&ctx->re_id_card);
        regfree(&ctx->re_phone);
    }
    pthread_rwlock_unlock(&ctx->rwlock);
    pthread_rwlock_destroy(&ctx->rwlock);
    free(ctx);
}

int
guardrails_load_rules(guardrails_ctx_t* ctx, const guardrail_rule_t* rules, size_t count)
{
    if (ctx == NULL) {
        return -1;
    }
    ac_trie_t* new_block = ac_trie_create();
    ac_trie_t* new_exempt = ac_trie_create();
    if (new_block == NULL || new_exempt == NULL) {
        ac_trie_destroy(new_block);
        ac_trie_destroy(new_exempt);
        return -1;
    }

    guardrail_webhook_rule_t temp_webhooks[MAX_WEBHOOK_RULES];
    size_t                   temp_webhook_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (!rules[i].enabled) {
            continue;
        }
        if (strcmp(rules[i].rule_type, "webhook") == 0) {
            if (temp_webhook_count < MAX_WEBHOOK_RULES) {
                guardrail_webhook_rule_t* wh = &temp_webhooks[temp_webhook_count++];
                wh->id = rules[i].id;
                strncpy(wh->url, rules[i].pattern, sizeof(wh->url) - 1);
                wh->url[sizeof(wh->url) - 1] = '\0';
                strncpy(wh->secret, rules[i].webhook_secret, sizeof(wh->secret) - 1);
                wh->secret[sizeof(wh->secret) - 1] = '\0';
                wh->timeout_ms = rules[i].timeout_ms > 0 ? rules[i].timeout_ms : 500;
                strncpy(wh->fail_mode,
                        rules[i].fail_mode[0] != '\0' ? rules[i].fail_mode : "open",
                        sizeof(wh->fail_mode) - 1);
                wh->fail_mode[sizeof(wh->fail_mode) - 1] = '\0';
                strncpy(wh->phase,
                        rules[i].phase[0] != '\0' ? rules[i].phase : "inbound",
                        sizeof(wh->phase) - 1);
                wh->phase[sizeof(wh->phase) - 1] = '\0';
            }
        } else if (strcmp(rules[i].rule_type, "keyword") == 0 ||
                   strcmp(rules[i].action, "block") == 0) {
            ac_trie_insert(new_block, rules[i].pattern);
        } else if (strcmp(rules[i].rule_type, "exempt") == 0) {
            ac_trie_insert(new_exempt, rules[i].pattern);
        }
    }
    ac_trie_build_failure_links(new_block);
    ac_trie_build_failure_links(new_exempt);

    pthread_rwlock_wrlock(&ctx->rwlock);
    ac_trie_destroy(ctx->ac_block);
    ac_trie_destroy(ctx->ac_exempt);
    ctx->ac_block = new_block;
    ctx->ac_exempt = new_exempt;
    memcpy(ctx->webhooks, temp_webhooks, sizeof(temp_webhooks[0]) * temp_webhook_count);
    ctx->webhook_count = temp_webhook_count;
    pthread_rwlock_unlock(&ctx->rwlock);
    return 0;
}

/** @brief Regex global replace: with digit-boundary check on, skip hits directly adjacent to digits on both sides (e.g. ID card/phone).
 *  @param out_changed Optional, always written with whether a replacement happened.
 *  @return New string (caller frees); NULL when src is NULL, src copy as OOM fallback. */
static char*
replace_regex(const regex_t* re,
              const char*    src,
              const char*    repl,
              int            check_digit_boundary,
              int*           out_changed)
{
    if (src == NULL) {
        return NULL;
    }
    regmatch_t  pmatch[1];
    const char* cursor = src;
    size_t      src_len = strlen(src);
    size_t      repl_len = strlen(repl);

    size_t cap = src_len + 64;
    char*  buf = malloc(cap);
    if (buf == NULL) {
        return strdup(src);
    }
    size_t len = 0;
    int    any_match = 0;

    while (*cursor != '\0') {
        if (regexec(re, cursor, 1, pmatch, 0) != 0) {
            size_t rem = strlen(cursor);
            if (len + rem + 1 > cap) {
                cap = len + rem + 64;
                char* nb = realloc(buf, cap);
                if (nb == NULL) {
                    free(buf);
                    return strdup(src);
                }
                buf = nb;
            }
            memcpy(buf + len, cursor, rem);
            len += rem;
            break;
        }

        regoff_t so = pmatch[0].rm_so;
        regoff_t eo = pmatch[0].rm_eo;

        if (check_digit_boundary) {
            int before_digit = (so > 0 && isdigit((unsigned char)cursor[so - 1]));
            int after_digit = (cursor[eo] != '\0' && isdigit((unsigned char)cursor[eo]));
            if (before_digit || after_digit) {
                size_t step = (size_t)so + 1;
                if (len + step + 1 > cap) {
                    cap = len + step + 64;
                    char* nb = realloc(buf, cap);
                    if (nb == NULL) {
                        free(buf);
                        return strdup(src);
                    }
                    buf = nb;
                }
                memcpy(buf + len, cursor, step);
                len += step;
                cursor += step;
                continue;
            }
        }

        if (len + (size_t)so + repl_len + 1 > cap) {
            cap = len + (size_t)so + repl_len + 64;
            char* nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                return strdup(src);
            }
            buf = nb;
        }
        memcpy(buf + len, cursor, (size_t)so);
        len += (size_t)so;

        memcpy(buf + len, repl, repl_len);
        len += repl_len;

        cursor += eo;
        any_match = 1;
    }

    buf[len] = '\0';
    if (any_match) {
        if (out_changed != NULL) {
            *out_changed = 1;
        }
        return buf;
    }
    free(buf);
    return strdup(src);
}

char*
guardrails_mask_pii_text(guardrails_ctx_t* ctx, const char* text, size_t len, int* changed)
{
    (void)len;
    if (ctx == NULL || text == NULL || text[0] == '\0') {
        if (changed != NULL) {
            *changed = 0;
        }
        return NULL;
    }

    int   local_changed = 0;
    char* s1 = replace_regex(&ctx->re_api_key, text, "[API_KEY]", 0, &local_changed);
    char* s2 = replace_regex(&ctx->re_email, s1, "[EMAIL]", 0, &local_changed);
    free(s1);
    char* s3 = replace_regex(&ctx->re_id_card, s2, "[ID_CARD]", 1, &local_changed);
    free(s2);
    char* s4 = replace_regex(&ctx->re_phone, s3, "[PHONE]", 1, &local_changed);
    free(s3);

    if (local_changed) {
        if (changed != NULL) {
            *changed = 1;
        }
        return s4;
    }
    free(s4);
    if (changed != NULL) {
        *changed = 0;
    }
    return NULL;
}

guardrails_action_t
guardrails_inspect_inbound(guardrails_ctx_t* ctx,
                           const char*       raw_body,
                           size_t            raw_len,
                           char**            sanitized_body,
                           size_t*           sanitized_len,
                           char*             blocked_keyword,
                           size_t            blocked_keyword_sz)
{
    if (ctx == NULL || raw_body == NULL || raw_len == 0) {
        if (sanitized_body != NULL) {
            *sanitized_body = NULL;
        }
        if (sanitized_len != NULL) {
            *sanitized_len = 0;
        }
        return GUARDRAILS_PASS;
    }

    pthread_rwlock_rdlock(&ctx->rwlock);

    /* 1. Fast AC Trie blocklist search */
    const char* kw = ac_trie_search(ctx->ac_block, raw_body, raw_len);
    if (kw != NULL) {
        /* Check exemption */
        const char* exempt = ac_trie_search(ctx->ac_exempt, raw_body, raw_len);
        if (exempt == NULL) {
            if (blocked_keyword != NULL && blocked_keyword_sz > 0) {
                strncpy(blocked_keyword, kw, blocked_keyword_sz - 1);
                blocked_keyword[blocked_keyword_sz - 1] = '\0';
            }
            pthread_rwlock_unlock(&ctx->rwlock);
            if (sanitized_body != NULL) {
                *sanitized_body = NULL;
            }
            if (sanitized_len != NULL) {
                *sanitized_len = 0;
            }
            return GUARDRAILS_BLOCKED;
        }
    }

    pthread_rwlock_unlock(&ctx->rwlock);

    /* 2. PII scanning & sanitization */
    json_error_t err;
    json_t*      root = json_loads(raw_body, 0, &err);
    if (root != NULL && json_is_object(root)) {
        int json_changed = 0;

        /* 2a. Inspect "messages" array */
        json_t* j_msgs = json_object_get(root, "messages");
        if (j_msgs != NULL && json_is_array(j_msgs)) {
            size_t  idx;
            json_t* msg;
            json_array_foreach(j_msgs, idx, msg)
            {
                if (!json_is_object(msg)) {
                    continue;
                }
                json_t* j_content = json_object_get(msg, "content");
                if (j_content != NULL && json_is_string(j_content)) {
                    int   c_changed = 0;
                    char* masked = guardrails_mask_pii_text(ctx,
                                                            json_string_value(j_content),
                                                            strlen(json_string_value(j_content)),
                                                            &c_changed);
                    if (c_changed && masked != NULL) {
                        json_object_set_new(msg, "content", json_string(masked));
                        free(masked);
                        json_changed = 1;
                    }
                } else if (j_content != NULL && json_is_array(j_content)) {
                    size_t  p_idx;
                    json_t* part;
                    json_array_foreach(j_content, p_idx, part)
                    {
                        if (!json_is_object(part)) {
                            continue;
                        }
                        json_t* j_text = json_object_get(part, "text");
                        if (j_text != NULL && json_is_string(j_text)) {
                            int   c_changed = 0;
                            char* masked =
                                guardrails_mask_pii_text(ctx,
                                                         json_string_value(j_text),
                                                         strlen(json_string_value(j_text)),
                                                         &c_changed);
                            if (c_changed && masked != NULL) {
                                json_object_set_new(part, "text", json_string(masked));
                                free(masked);
                                json_changed = 1;
                            }
                        }
                    }
                }
            }
        }

        /* 2b. Inspect "prompt" */
        json_t* j_prompt = json_object_get(root, "prompt");
        if (j_prompt != NULL && json_is_string(j_prompt)) {
            int   c_changed = 0;
            char* masked = guardrails_mask_pii_text(
                ctx, json_string_value(j_prompt), strlen(json_string_value(j_prompt)), &c_changed);
            if (c_changed && masked != NULL) {
                json_object_set_new(root, "prompt", json_string(masked));
                free(masked);
                json_changed = 1;
            }
        }

        /* 2c. Inspect "system" */
        json_t* j_sys = json_object_get(root, "system");
        if (j_sys != NULL && json_is_string(j_sys)) {
            int   c_changed = 0;
            char* masked = guardrails_mask_pii_text(
                ctx, json_string_value(j_sys), strlen(json_string_value(j_sys)), &c_changed);
            if (c_changed && masked != NULL) {
                json_object_set_new(root, "system", json_string(masked));
                free(masked);
                json_changed = 1;
            }
        }

        /* 2d. Inspect "contents" (Gemini format) */
        json_t* j_contents = json_object_get(root, "contents");
        if (j_contents != NULL && json_is_array(j_contents)) {
            size_t  c_idx;
            json_t* c_item;
            json_array_foreach(j_contents, c_idx, c_item)
            {
                if (!json_is_object(c_item)) {
                    continue;
                }
                json_t* j_parts = json_object_get(c_item, "parts");
                if (j_parts != NULL && json_is_array(j_parts)) {
                    size_t  p_idx;
                    json_t* part;
                    json_array_foreach(j_parts, p_idx, part)
                    {
                        if (!json_is_object(part)) {
                            continue;
                        }
                        json_t* j_text = json_object_get(part, "text");
                        if (j_text != NULL && json_is_string(j_text)) {
                            int   c_changed = 0;
                            char* masked =
                                guardrails_mask_pii_text(ctx,
                                                         json_string_value(j_text),
                                                         strlen(json_string_value(j_text)),
                                                         &c_changed);
                            if (c_changed && masked != NULL) {
                                json_object_set_new(part, "text", json_string(masked));
                                free(masked);
                                json_changed = 1;
                            }
                        }
                    }
                }
            }
        }

        if (json_changed) {
            char* dumped = json_dumps(root, JSON_COMPACT);
            json_decref(root);
            if (dumped != NULL) {
                if (sanitized_body != NULL) {
                    *sanitized_body = dumped;
                }
                if (sanitized_len != NULL) {
                    *sanitized_len = strlen(dumped);
                }
                return GUARDRAILS_MASKED;
            }
        } else {
            json_decref(root);
        }
    } else {
        if (root != NULL) {
            json_decref(root);
        }
        /* Fallback for non-JSON text payloads */
        int   c_changed = 0;
        char* masked = guardrails_mask_pii_text(ctx, raw_body, raw_len, &c_changed);
        if (c_changed && masked != NULL) {
            if (sanitized_body != NULL) {
                *sanitized_body = masked;
            }
            if (sanitized_len != NULL) {
                *sanitized_len = strlen(masked);
            }
            return GUARDRAILS_MASKED;
        }
    }

    if (sanitized_body != NULL) {
        *sanitized_body = NULL;
    }
    if (sanitized_len != NULL) {
        *sanitized_len = 0;
    }
    return GUARDRAILS_PASS;
}

/* --- External Webhook Moderation Engine Implementation --- */

/** @brief Dynamic memory buffer accumulating webhook response body chunks. */
struct webhook_resp_buf {
    char*  data; /**< Dynamically allocated response buffer. */
    size_t len;  /**< Current response length in bytes. */
    size_t cap;  /**< Allocated buffer capacity in bytes. */
};

static size_t
webhook_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t                   total = size * nmemb;
    struct webhook_resp_buf* b = userdata;
    if (b->len + total > 2 * 1024 * 1024) {
        return 0;
    }
    if (b->len + total + 1 > b->cap) {
        size_t ncap = (b->cap == 0 ? 2048 : b->cap * 2) + total;
        char*  nd = realloc(b->data, ncap);
        if (nd == NULL) {
            return 0;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static void
extract_inbound_info(json_t* root, const char* raw_body, char** out_content, json_t** out_msgs)
{
    *out_content = NULL;
    *out_msgs = NULL;

    if (root != NULL && json_is_object(root)) {
        json_t* j_msgs = json_object_get(root, "messages");
        if (j_msgs != NULL && json_is_array(j_msgs)) {
            *out_msgs = j_msgs;
            size_t n = json_array_size(j_msgs);
            for (size_t i = n; i > 0; i--) {
                json_t* m = json_array_get(j_msgs, i - 1);
                if (!json_is_object(m)) {
                    continue;
                }
                json_t* r = json_object_get(m, "role");
                if (r != NULL && json_is_string(r) && strcmp(json_string_value(r), "user") == 0) {
                    json_t* c = json_object_get(m, "content");
                    if (c != NULL && json_is_string(c)) {
                        *out_content = strdup(json_string_value(c));
                        return;
                    }
                    if (c != NULL && json_is_array(c)) {
                        size_t pn = json_array_size(c);
                        for (size_t pi = 0; pi < pn; pi++) {
                            json_t* part = json_array_get(c, pi);
                            json_t* txt = json_object_get(part, "text");
                            if (txt != NULL && json_is_string(txt)) {
                                *out_content = strdup(json_string_value(txt));
                                return;
                            }
                        }
                    }
                }
            }
        }
        json_t* j_prompt = json_object_get(root, "prompt");
        if (j_prompt != NULL && json_is_string(j_prompt)) {
            *out_content = strdup(json_string_value(j_prompt));
            return;
        }
        json_t* j_contents = json_object_get(root, "contents");
        if (j_contents != NULL && json_is_array(j_contents)) {
            size_t n = json_array_size(j_contents);
            for (size_t i = n; i > 0; i--) {
                json_t* item = json_array_get(j_contents, i - 1);
                json_t* parts = json_object_get(item, "parts");
                if (parts != NULL && json_is_array(parts)) {
                    size_t pn = json_array_size(parts);
                    for (size_t pi = 0; pi < pn; pi++) {
                        json_t* p = json_array_get(parts, pi);
                        json_t* txt = json_object_get(p, "text");
                        if (txt != NULL && json_is_string(txt)) {
                            *out_content = strdup(json_string_value(txt));
                            return;
                        }
                    }
                }
            }
        }
    }

    if (*out_content == NULL && raw_body != NULL) {
        *out_content = strdup(raw_body);
    }
}

static char*
apply_inbound_mask(const char* body, const char* masked_content)
{
    json_error_t err;
    json_t*      root = json_loads(body, 0, &err);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return strdup(masked_content);
    }

    int     replaced = 0;
    json_t* j_msgs = json_object_get(root, "messages");
    if (j_msgs != NULL && json_is_array(j_msgs)) {
        size_t n = json_array_size(j_msgs);
        for (size_t i = n; i > 0; i--) {
            json_t* m = json_array_get(j_msgs, i - 1);
            if (!json_is_object(m)) {
                continue;
            }
            json_t* r = json_object_get(m, "role");
            if (r != NULL && json_is_string(r) && strcmp(json_string_value(r), "user") == 0) {
                json_object_set_new(m, "content", json_string(masked_content));
                replaced = 1;
                break;
            }
        }
        if (!replaced && n > 0) {
            json_t* m = json_array_get(j_msgs, n - 1);
            if (json_is_object(m)) {
                json_object_set_new(m, "content", json_string(masked_content));
                replaced = 1;
            }
        }
    }
    if (!replaced) {
        json_t* j_prompt = json_object_get(root, "prompt");
        if (j_prompt != NULL) {
            json_object_set_new(root, "prompt", json_string(masked_content));
            replaced = 1;
        }
    }
    if (!replaced) {
        json_t* j_contents = json_object_get(root, "contents");
        if (j_contents != NULL && json_is_array(j_contents)) {
            size_t n = json_array_size(j_contents);
            if (n > 0) {
                json_t* item = json_array_get(j_contents, n - 1);
                json_t* parts = json_object_get(item, "parts");
                if (parts != NULL && json_is_array(parts) && json_array_size(parts) > 0) {
                    json_t* p = json_array_get(parts, 0);
                    if (json_is_object(p)) {
                        json_object_set_new(p, "text", json_string(masked_content));
                        replaced = 1;
                    }
                }
            }
        }
    }

    char* res = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return res != NULL ? res : strdup(masked_content);
}

static char*
extract_outbound_content(json_t* root, const char* response_body)
{
    if (root != NULL && json_is_object(root)) {
        json_t* choices = json_object_get(root, "choices");
        if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
            json_t* c0 = json_array_get(choices, 0);
            if (json_is_object(c0)) {
                json_t* msg = json_object_get(c0, "message");
                if (msg != NULL && json_is_object(msg)) {
                    json_t* cont = json_object_get(msg, "content");
                    if (cont != NULL && json_is_string(cont)) {
                        return strdup(json_string_value(cont));
                    }
                }
                json_t* txt = json_object_get(c0, "text");
                if (txt != NULL && json_is_string(txt)) {
                    return strdup(json_string_value(txt));
                }
            }
        }
    }
    return response_body != NULL ? strdup(response_body) : strdup("");
}

static char*
apply_outbound_mask(const char* response_body, const char* masked_content)
{
    json_error_t err;
    json_t*      root = json_loads(response_body, 0, &err);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return strdup(masked_content);
    }
    json_t* choices = json_object_get(root, "choices");
    if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
        json_t* c0 = json_array_get(choices, 0);
        if (json_is_object(c0)) {
            json_t* msg = json_object_get(c0, "message");
            if (msg != NULL && json_is_object(msg)) {
                json_object_set_new(msg, "content", json_string(masked_content));
            } else {
                json_t* txt = json_object_get(c0, "text");
                if (txt != NULL) {
                    json_object_set_new(c0, "text", json_string(masked_content));
                }
            }
        }
    }
    char* res = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return res != NULL ? res : strdup(masked_content);
}

static guardrails_action_t
execute_single_webhook(const guardrail_webhook_rule_t* rule,
                       const char*                     phase,
                       const char*                     model,
                       long                            key_id,
                       const char*                     content,
                       json_t*                         messages_arr,
                       char**                          out_masked_content,
                       char*                           block_reason,
                       size_t                          block_reason_sz)
{
    if (out_masked_content != NULL) {
        *out_masked_content = NULL;
    }

    json_t* req_obj = json_object();
    json_object_set_new(req_obj, "phase", json_string(phase));
    json_object_set_new(req_obj, "model", json_string(model != NULL ? model : ""));
    json_object_set_new(req_obj, "key_id", json_integer((json_int_t)key_id));
    json_object_set_new(req_obj, "content", json_string(content != NULL ? content : ""));
    if (messages_arr != NULL) {
        json_object_set(req_obj, "messages", messages_arr);
    } else {
        json_object_set_new(req_obj, "messages", json_null());
    }
    json_object_set_new(req_obj, "timestamp", json_integer((json_int_t)time(NULL)));

    char* req_str = json_dumps(req_obj, JSON_COMPACT);
    json_decref(req_obj);
    if (req_str == NULL) {
        return GUARDRAILS_PASS;
    }

    CURL* c = curl_easy_init();
    if (c == NULL) {
        free(req_str);
        if (strcasecmp(rule->fail_mode, "closed") == 0) {
            if (block_reason != NULL && block_reason_sz > 0) {
                snprintf(block_reason, block_reason_sz, "moderation_curl_init_failed");
            }
            return GUARDRAILS_BLOCKED;
        }
        return GUARDRAILS_PASS;
    }

    struct curl_slist* hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (rule->secret[0] != '\0') {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", rule->secret);
        hdrs = curl_slist_append(hdrs, auth);
    }

    struct webhook_resp_buf rb = {0};

    curl_easy_setopt(c, CURLOPT_URL, rule->url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req_str);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(req_str));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, webhook_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(rule->timeout_ms > 0 ? rule->timeout_ms : 500));
    long conn_timeout =
        rule->timeout_ms > 1000 ? 1000 : (rule->timeout_ms > 0 ? rule->timeout_ms : 500);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, conn_timeout);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    CURLcode cret = curl_easy_perform(c);
    long     http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(req_str);

    guardrails_action_t act = GUARDRAILS_PASS;
    if (cret != CURLE_OK || http_code < 200 || http_code >= 300) {
        if (cret == CURLE_OPERATION_TIMEDOUT) {
            AIGATE_LOG_WARN(
                "guardrails webhook [%s] timed out (%d ms)", rule->url, rule->timeout_ms);
        } else {
            AIGATE_LOG_WARN("guardrails webhook [%s] error: %s (http %ld)",
                            rule->url,
                            curl_easy_strerror(cret),
                            http_code);
        }
        if (strcasecmp(rule->fail_mode, "closed") == 0) {
            if (block_reason != NULL && block_reason_sz > 0) {
                snprintf(block_reason, block_reason_sz, "moderation_webhook_unavailable");
            }
            act = GUARDRAILS_BLOCKED;
        } else {
            act = GUARDRAILS_PASS;
        }
        free(rb.data);
        return act;
    }

    json_error_t jerr;
    json_t*      resp_json = json_loads(rb.data != NULL ? rb.data : "", 0, &jerr);
    free(rb.data);

    if (resp_json == NULL || !json_is_object(resp_json)) {
        if (resp_json != NULL) {
            json_decref(resp_json);
        }
        AIGATE_LOG_WARN("guardrails webhook [%s] returned invalid JSON: %s", rule->url, jerr.text);
        if (strcasecmp(rule->fail_mode, "closed") == 0) {
            if (block_reason != NULL && block_reason_sz > 0) {
                snprintf(block_reason, block_reason_sz, "moderation_webhook_invalid_response");
            }
            return GUARDRAILS_BLOCKED;
        }
        return GUARDRAILS_PASS;
    }

    json_t*     j_act = json_object_get(resp_json, "action");
    const char* act_str =
        (j_act != NULL && json_is_string(j_act)) ? json_string_value(j_act) : "pass";

    if (strcasecmp(act_str, "block") == 0) {
        json_t*     j_reason = json_object_get(resp_json, "reason");
        const char* reason_str = (j_reason != NULL && json_is_string(j_reason))
                                     ? json_string_value(j_reason)
                                     : "content_policy_violation";
        if (block_reason != NULL && block_reason_sz > 0) {
            snprintf(block_reason, block_reason_sz, "%s", reason_str);
        }
        act = GUARDRAILS_BLOCKED;
    } else if (strcasecmp(act_str, "mask") == 0) {
        json_t* j_masked = json_object_get(resp_json, "masked_content");
        if (j_masked != NULL && json_is_string(j_masked)) {
            if (out_masked_content != NULL) {
                *out_masked_content = strdup(json_string_value(j_masked));
            }
            act = GUARDRAILS_MASKED;
        } else {
            act = GUARDRAILS_PASS;
        }
    } else {
        act = GUARDRAILS_PASS;
    }

    json_decref(resp_json);
    return act;
}

guardrails_action_t
guardrails_inspect_webhook_inbound(guardrails_ctx_t* ctx,
                                   const char*       model,
                                   long              key_id,
                                   const char*       raw_body,
                                   size_t            raw_len,
                                   char**            sanitized_body,
                                   size_t*           sanitized_len,
                                   char*             block_reason,
                                   size_t            block_reason_sz)
{
    if (sanitized_body != NULL) {
        *sanitized_body = NULL;
    }
    if (sanitized_len != NULL) {
        *sanitized_len = 0;
    }
    if (block_reason != NULL && block_reason_sz > 0) {
        block_reason[0] = '\0';
    }
    if (ctx == NULL || raw_body == NULL || raw_len == 0) {
        return GUARDRAILS_PASS;
    }

    guardrail_webhook_rule_t rules_copy[MAX_WEBHOOK_RULES];
    size_t                   n_rules = 0;

    pthread_rwlock_rdlock(&ctx->rwlock);
    for (size_t i = 0; i < ctx->webhook_count; i++) {
        const guardrail_webhook_rule_t* r = &ctx->webhooks[i];
        if (strcasecmp(r->phase, "inbound") == 0 || strcasecmp(r->phase, "both") == 0) {
            if (n_rules < MAX_WEBHOOK_RULES) {
                rules_copy[n_rules++] = *r;
            }
        }
    }
    pthread_rwlock_unlock(&ctx->rwlock);

    if (n_rules == 0) {
        return GUARDRAILS_PASS;
    }

    const char* current_body = raw_body;
    char*       cur_sanitized = NULL;
    int         any_masked = 0;

    for (size_t i = 0; i < n_rules; i++) {
        json_error_t jerr;
        json_t*      root = json_loads(current_body, 0, &jerr);
        char*        content = NULL;
        json_t*      msgs = NULL;
        extract_inbound_info(root, current_body, &content, &msgs);

        char*               masked_text = NULL;
        guardrails_action_t act = execute_single_webhook(&rules_copy[i],
                                                         "inbound",
                                                         model,
                                                         key_id,
                                                         content,
                                                         msgs,
                                                         &masked_text,
                                                         block_reason,
                                                         block_reason_sz);
        free(content);
        if (root != NULL) {
            json_decref(root);
        }

        if (act == GUARDRAILS_BLOCKED) {
            free(masked_text);
            free(cur_sanitized);
            return GUARDRAILS_BLOCKED;
        }
        if (act == GUARDRAILS_MASKED && masked_text != NULL) {
            char* next_body = apply_inbound_mask(current_body, masked_text);
            free(masked_text);
            free(cur_sanitized);
            cur_sanitized = next_body;
            current_body = cur_sanitized;
            any_masked = 1;
        }
    }

    if (any_masked && cur_sanitized != NULL) {
        if (sanitized_body != NULL) {
            *sanitized_body = cur_sanitized;
        } else {
            free(cur_sanitized);
        }
        if (sanitized_len != NULL) {
            *sanitized_len = cur_sanitized != NULL ? strlen(cur_sanitized) : 0;
        }
        return GUARDRAILS_MASKED;
    }

    free(cur_sanitized);
    return GUARDRAILS_PASS;
}

guardrails_action_t
guardrails_inspect_webhook_outbound(guardrails_ctx_t* ctx,
                                    const char*       model,
                                    long              key_id,
                                    const char*       response_body,
                                    size_t            response_len,
                                    char**            sanitized_body,
                                    size_t*           sanitized_len,
                                    char*             block_reason,
                                    size_t            block_reason_sz)
{
    if (sanitized_body != NULL) {
        *sanitized_body = NULL;
    }
    if (sanitized_len != NULL) {
        *sanitized_len = 0;
    }
    if (block_reason != NULL && block_reason_sz > 0) {
        block_reason[0] = '\0';
    }
    if (ctx == NULL || response_body == NULL || response_len == 0) {
        return GUARDRAILS_PASS;
    }

    guardrail_webhook_rule_t rules_copy[MAX_WEBHOOK_RULES];
    size_t                   n_rules = 0;

    pthread_rwlock_rdlock(&ctx->rwlock);
    for (size_t i = 0; i < ctx->webhook_count; i++) {
        const guardrail_webhook_rule_t* r = &ctx->webhooks[i];
        if (strcasecmp(r->phase, "outbound") == 0 || strcasecmp(r->phase, "both") == 0) {
            if (n_rules < MAX_WEBHOOK_RULES) {
                rules_copy[n_rules++] = *r;
            }
        }
    }
    pthread_rwlock_unlock(&ctx->rwlock);

    if (n_rules == 0) {
        return GUARDRAILS_PASS;
    }

    const char* current_body = response_body;
    char*       cur_sanitized = NULL;
    int         any_masked = 0;

    for (size_t i = 0; i < n_rules; i++) {
        json_error_t jerr;
        json_t*      root = json_loads(current_body, 0, &jerr);
        char*        content = extract_outbound_content(root, current_body);

        char*               masked_text = NULL;
        guardrails_action_t act = execute_single_webhook(&rules_copy[i],
                                                         "outbound",
                                                         model,
                                                         key_id,
                                                         content,
                                                         NULL,
                                                         &masked_text,
                                                         block_reason,
                                                         block_reason_sz);
        free(content);
        if (root != NULL) {
            json_decref(root);
        }

        if (act == GUARDRAILS_BLOCKED) {
            free(masked_text);
            free(cur_sanitized);
            return GUARDRAILS_BLOCKED;
        }
        if (act == GUARDRAILS_MASKED && masked_text != NULL) {
            char* next_body = apply_outbound_mask(current_body, masked_text);
            free(masked_text);
            free(cur_sanitized);
            cur_sanitized = next_body;
            current_body = cur_sanitized;
            any_masked = 1;
        }
    }

    if (any_masked && cur_sanitized != NULL) {
        if (sanitized_body != NULL) {
            *sanitized_body = cur_sanitized;
        } else {
            free(cur_sanitized);
        }
        if (sanitized_len != NULL) {
            *sanitized_len = cur_sanitized != NULL ? strlen(cur_sanitized) : 0;
        }
        return GUARDRAILS_MASKED;
    }

    free(cur_sanitized);
    return GUARDRAILS_PASS;
}

int
guardrails_webhook_probe(const char* url,
                         const char* secret,
                         int         timeout_ms,
                         char*       out_err,
                         size_t      err_sz,
                         double*     out_latency_ms)
{
    if (out_err != NULL && err_sz > 0) {
        out_err[0] = '\0';
    }
    if (out_latency_ms != NULL) {
        *out_latency_ms = 0.0;
    }
    if (url == NULL || url[0] == '\0') {
        if (out_err != NULL && err_sz > 0) {
            snprintf(out_err, err_sz, "Invalid empty URL");
        }
        return -1;
    }

    CURL* c = curl_easy_init();
    if (c == NULL) {
        if (out_err != NULL && err_sz > 0) {
            snprintf(out_err, err_sz, "Failed to initialize CURL");
        }
        return -1;
    }

    json_t* req = json_object();
    json_object_set_new(req, "phase", json_string("probe"));
    json_object_set_new(req, "model", json_string("probe-test"));
    json_object_set_new(req, "key_id", json_integer(0));
    json_object_set_new(req, "content", json_string("AIGate connection probe test"));
    json_object_set_new(req, "timestamp", json_integer((json_int_t)time(NULL)));
    char* req_str = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (req_str == NULL) {
        curl_easy_cleanup(c);
        if (out_err != NULL && err_sz > 0) {
            snprintf(out_err, err_sz, "Failed to serialize test JSON");
        }
        return -1;
    }

    struct curl_slist* hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (secret != NULL && secret[0] != '\0') {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", secret);
        hdrs = curl_slist_append(hdrs, auth);
    }

    struct webhook_resp_buf rb = {0};

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req_str);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(req_str));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, webhook_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(timeout_ms > 0 ? timeout_ms : 3000));
    long conn_timeout = timeout_ms > 1000 ? 1000 : (timeout_ms > 0 ? timeout_ms : 1000);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, conn_timeout);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    CURLcode cret = curl_easy_perform(c);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double latency = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;
    if (out_latency_ms != NULL) {
        *out_latency_ms = latency;
    }

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(req_str);
    free(rb.data);

    if (cret != CURLE_OK) {
        if (out_err != NULL && err_sz > 0) {
            snprintf(out_err, err_sz, "Connection failed: %s", curl_easy_strerror(cret));
        }
        return -1;
    }
    if (http_code < 200 || http_code >= 300) {
        if (out_err != NULL && err_sz > 0) {
            snprintf(out_err, err_sz, "Webhook returned HTTP %ld", http_code);
        }
        return -1;
    }

    return 0;
}

bool
guardrails_validate_luhn(const char* digits)
{
    if (digits == NULL) {
        return false;
    }
    size_t len = strlen(digits);
    if (len < 13 || len > 19) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)digits[i])) {
            return false;
        }
    }
    int  sum = 0;
    bool alternate = false;
    for (ssize_t i = (ssize_t)len - 1; i >= 0; i--) {
        int n = digits[i] - '0';
        if (alternate) {
            n *= 2;
            if (n > 9) {
                n = (n % 10) + 1;
            }
        }
        sum += n;
        alternate = !alternate;
    }
    return (sum % 10 == 0);
}

bool
guardrails_validate_id_card_mod11(const char* id_str)
{
    if (id_str == NULL) {
        return false;
    }
    if (strlen(id_str) != 18) {
        return false;
    }
    static const int  weights[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    static const char check_chars[11] = {'1', '0', 'X', '9', '8', '7', '6', '5', '4', '3', '2'};

    int sum = 0;
    for (int i = 0; i < 17; i++) {
        if (!isdigit((unsigned char)id_str[i])) {
            return false;
        }
        sum += (id_str[i] - '0') * weights[i];
    }
    int  mod = sum % 11;
    char expected = check_chars[mod];
    char actual = id_str[17];
    if (actual == 'x') {
        actual = 'X';
    }
    return actual == expected;
}
