/** @file guardrails.h
 *  @brief Content moderation & guardrails: Aho-Corasick multi-pattern keyword matching
 *         and inbound PII masking pipeline.
 */

/**
 * @defgroup group_policy 策略层
 * @brief 策略：鉴权、预算、熔断、护栏、限流、响应缓存。
 */
#ifndef AIGATE_GUARDRAILS_H
#define AIGATE_GUARDRAILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "pg_store.h"

/** @brief 入站检查结果：放行/已脱敏放行/拦截。 */
typedef enum {
    GUARDRAILS_PASS = 0,     /* Content clean, no masking or blocking */
    GUARDRAILS_MASKED = 1,   /* PII found and masked */
    GUARDRAILS_BLOCKED = 2   /* Forbidden keyword found, request blocked */
} guardrails_action_t;

/* --- Aho-Corasick Pattern Matching Trie --- */

/** @brief AC 自动机节点：转移表/失败链接/命中关键词。 */
/** @brief AC 自动机节点：转移表/失败链接/命中关键词。 */
typedef struct ac_node {
    int   next[256];          /**< Transition on byte 0..255; -1 = uninitialized */
    int   fail;               /**< Failure link index */
    char* matched_keyword;    /**< Keyword ending at this node (or NULL) */
} ac_node_t;

/** @brief AC 自动机：节点池 + 已用数 + 容量。 */
typedef struct ac_trie {
    ac_node_t* nodes; /**< 节点池（索引 0 为根） */
    size_t     node_count; /**< 已用节点数 */
    size_t     node_cap; /**< 节点池容量 */
} ac_trie_t;

/** @brief 新建 AC 自动机（空 trie，仅根节点）。
 *  @return 新实例；OOM 返回 NULL。 */
ac_trie_t*  ac_trie_create(void);
/** @brief 释放 trie 及其全部节点关键词（NULL 安全）。 */
void        ac_trie_destroy(ac_trie_t* trie);
/** @brief 插入一个关键词（空串/NULL 拒绝）。
 *  @return 0 成功；-1 参数非法或 OOM。 */
int         ac_trie_insert(ac_trie_t* trie, const char* keyword);
/** @brief BFS 构建失败链接并补全转移表（调用 insert 后、search 前必须调用一次）。
 *  @return 0 成功（含空 trie）；-1 OOM。 */
int         ac_trie_build_failure_links(ac_trie_t* trie);
/** @brief 在文本中搜首个命中关键词。
 *  @return 命中关键词（借用指针，勿释放）；无命中/空 trie 返回 NULL。 */
const char* ac_trie_search(const ac_trie_t* trie, const char* text, size_t len);

/* --- Full Guardrails Engine --- */

/** @brief 护栏引擎实例（不透明，定义见 guardrails.c）。 */
typedef struct guardrails_ctx guardrails_ctx_t;

/** @brief 新建护栏引擎（编译 PII 正则，空规则集）。
 *  @return 新实例；OOM 返回 NULL。 */
guardrails_ctx_t* guardrails_create(void);
/** @brief 释放引擎（含两棵 AC trie 与正则，NULL 安全）。 */
void              guardrails_destroy(guardrails_ctx_t* ctx);
/** @brief 全量替换规则集（重建 block/exempt 两棵 trie 并 build 失败链接）。
 *  @return 0 成功；-1 OOM（旧规则保留）。 */
int               guardrails_load_rules(guardrails_ctx_t* ctx, const guardrail_rule_t* rules, size_t count);

/** @brief 入站检查：先 AC 黑名单（命中且无豁免 → BLOCKED 并回填关键词），再 PII 脱敏（命中 → MASKED 并输出脱敏体），否则 PASS。
 *  @return GUARDRAILS_PASS/MASKED/BLOCKED 三者之一。 */
guardrails_action_t guardrails_inspect_inbound(
    guardrails_ctx_t* ctx,
    const char*       raw_body,
    size_t            raw_len,
    char**            sanitized_body,
    size_t*           sanitized_len,
    char*             blocked_keyword,
    size_t            blocked_keyword_sz);

/** @brief PII 脱敏：API key→[API_KEY]、邮箱→[EMAIL]、身份证→[ID_CARD]、电话→[PHONE]。
 *  @param ctx 护栏引擎实例。
 *  @param text 待脱敏文本（不要求 NUL 结尾）。
 *  @param len 文本长度。
 *  @param changed 可选，恒写是否发生替换。
 *  @return 脱敏后新串（调用方 free）；无命中/空输入返回 NULL。 */
char* guardrails_mask_pii_text(guardrails_ctx_t* ctx, const char* text, size_t len, int* changed);

#endif /* AIGATE_GUARDRAILS_H */
