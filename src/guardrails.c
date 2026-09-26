/** @file guardrails.c
 *  @brief Content moderation & guardrails implementation:
 *         Aho-Corasick multi-pattern trie and inbound inspection.
 */
#include "guardrails.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
            return GUARDRAILS_BLOCKED;
        }
    }

    pthread_rwlock_unlock(&ctx->rwlock);

    /* Task 3 will implement PII scanning here */
    if (sanitized_body != NULL) {
        *sanitized_body = NULL;
    }
    if (sanitized_len != NULL) {
        *sanitized_len = 0;
    }
    return GUARDRAILS_PASS;
}
