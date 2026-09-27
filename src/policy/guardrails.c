/** @file guardrails.c
 *  @brief Content moderation & guardrails implementation:
 *         Aho-Corasick multi-pattern trie, POSIX regex PII scanning,
 *         and inbound payload sanitization.
 */
#include "guardrails.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <regex.h>
#include <ctype.h>
#include <jansson.h>

#define AC_INIT_CAP 256

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
    for (size_t i = 0; keyword[i] != '\0'; i++) {
        unsigned char c = (unsigned char)keyword[i];
        if (trie->nodes[curr].next[c] == -1) {
            if (trie->node_count >= trie->node_cap) {
                size_t new_cap = trie->node_cap * 2;
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

    /* Setup level 1 transitions and root failure links */
    for (int c = 0; c < 256; c++) {
        int next_node = trie->nodes[0].next[c];
        if (next_node != -1) {
            trie->nodes[next_node].fail = 0;
            queue[q_tail++] = next_node;
        } else {
            trie->nodes[0].next[c] = 0;
        }
    }

    /* BFS */
    while (q_head < q_tail) {
        int r = queue[q_head++];
        for (int c = 0; c < 256; c++) {
            int u = trie->nodes[r].next[c];
            if (u != -1) {
                int fail_state = trie->nodes[r].fail;
                trie->nodes[u].fail = trie->nodes[fail_state].next[c];

                /* Propagate matched keyword if failure state matches and current node doesn't have one */
                if (trie->nodes[u].matched_keyword == NULL &&
                    trie->nodes[trie->nodes[u].fail].matched_keyword != NULL) {
                    trie->nodes[u].matched_keyword =
                        strdup(trie->nodes[trie->nodes[u].fail].matched_keyword);
                }

                queue[q_tail++] = u;
            } else {
                /* DFA optimization: transition to failure state's transition */
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
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        state = trie->nodes[state].next[c];
        if (trie->nodes[state].matched_keyword != NULL) {
            return trie->nodes[state].matched_keyword;
        }
    }
    return NULL;
}

/* --- Guardrails Engine Context --- */

struct guardrails_ctx {
    pthread_rwlock_t rwlock;
    ac_trie_t*       ac_block;
    ac_trie_t*       ac_exempt;
    regex_t          re_api_key;
    regex_t          re_email;
    regex_t          re_id_card;
    regex_t          re_phone;
    int              regex_ready;
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
    regcomp(&ctx->re_api_key, "sk-[a-zA-Z0-9]{20,}|aig_[a-zA-Z0-9]{20,}|ghp_[a-zA-Z0-9]{20,}", REG_EXTENDED);
    regcomp(&ctx->re_email, "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}", REG_EXTENDED);
    regcomp(&ctx->re_id_card, "[1-9][0-9]{5}(18|19|20)[0-9]{2}(0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])[0-9]{3}[0-9Xx]", REG_EXTENDED);
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

    for (size_t i = 0; i < count; i++) {
        if (!rules[i].enabled) {
            continue;
        }
        if (strcmp(rules[i].rule_type, "keyword") == 0 || strcmp(rules[i].action, "block") == 0) {
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
    pthread_rwlock_unlock(&ctx->rwlock);
    return 0;
}

static char*
replace_regex(const regex_t* re, const char* src, const char* repl, int check_digit_boundary, int* out_changed)
{
    if (src == NULL) {
        return NULL;
    }
    regmatch_t pmatch[1];
    const char* cursor = src;
    size_t src_len = strlen(src);
    size_t repl_len = strlen(repl);

    size_t cap = src_len + 64;
    char* buf = malloc(cap);
    if (buf == NULL) {
        return strdup(src);
    }
    size_t len = 0;
    int any_match = 0;

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

    int local_changed = 0;
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
guardrails_inspect_inbound(
    guardrails_ctx_t* ctx,
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
    json_t* root = json_loads(raw_body, 0, &err);
    if (root != NULL && json_is_object(root)) {
        int json_changed = 0;

        /* 2a. Inspect "messages" array */
        json_t* j_msgs = json_object_get(root, "messages");
        if (j_msgs != NULL && json_is_array(j_msgs)) {
            size_t idx;
            json_t* msg;
            json_array_foreach(j_msgs, idx, msg) {
                if (!json_is_object(msg)) {
                    continue;
                }
                json_t* j_content = json_object_get(msg, "content");
                if (j_content != NULL && json_is_string(j_content)) {
                    int c_changed = 0;
                    char* masked = guardrails_mask_pii_text(
                        ctx, json_string_value(j_content), strlen(json_string_value(j_content)), &c_changed);
                    if (c_changed && masked != NULL) {
                        json_object_set_new(msg, "content", json_string(masked));
                        free(masked);
                        json_changed = 1;
                    }
                } else if (j_content != NULL && json_is_array(j_content)) {
                    size_t p_idx;
                    json_t* part;
                    json_array_foreach(j_content, p_idx, part) {
                        if (!json_is_object(part)) {
                            continue;
                        }
                        json_t* j_text = json_object_get(part, "text");
                        if (j_text != NULL && json_is_string(j_text)) {
                            int c_changed = 0;
                            char* masked = guardrails_mask_pii_text(
                                ctx, json_string_value(j_text), strlen(json_string_value(j_text)), &c_changed);
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
            int c_changed = 0;
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
            int c_changed = 0;
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
            size_t c_idx;
            json_t* c_item;
            json_array_foreach(j_contents, c_idx, c_item) {
                if (!json_is_object(c_item)) {
                    continue;
                }
                json_t* j_parts = json_object_get(c_item, "parts");
                if (j_parts != NULL && json_is_array(j_parts)) {
                    size_t p_idx;
                    json_t* part;
                    json_array_foreach(j_parts, p_idx, part) {
                        if (!json_is_object(part)) {
                            continue;
                        }
                        json_t* j_text = json_object_get(part, "text");
                        if (j_text != NULL && json_is_string(j_text)) {
                            int c_changed = 0;
                            char* masked = guardrails_mask_pii_text(
                                ctx, json_string_value(j_text), strlen(json_string_value(j_text)), &c_changed);
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
        int c_changed = 0;
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
