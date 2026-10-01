#include "filter_chain.h"
#include "guardrails.h"
#include "prompt_template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Inbound Filter 1: Content moderation & PII scanning.
 */
static filter_action_t
filter_guardrails(chat_req_t* q)
{
    char matched_rule[128] = {0};
    q->sanitized_body = NULL;
    q->sanitized_len = 0;
    q->eff_body = q->rq->body;
    q->eff_len = q->rq->body_len;

    if (q->krec.guardrails_enabled && q->ac->gr != NULL && q->rq->body != NULL &&
        q->rq->body_len > 0) {
        /* L1: Local Aho-Corasick & PII Regex */
        guardrails_action_t gr_res = guardrails_inspect_inbound_with_pii(q->ac->gr,
                                                                         (const char*)q->rq->body,
                                                                         q->rq->body_len,
                                                                         &q->pii_map,
                                                                         &q->sanitized_body,
                                                                         &q->sanitized_len,
                                                                         matched_rule,
                                                                         sizeof matched_rule);
        if (gr_res == GUARDRAILS_BLOCKED) {
            char block_msg[256];
            snprintf(block_msg,
                     sizeof block_msg,
                     "Blocked by safety guardrail rule: %s",
                     matched_rule[0] ? matched_rule : "blocked content");
            aigate_write_error(q->rc, 400, "content_policy_violation", block_msg);
            record_usage_and_event(
                q->ac, q->krec.key_id, q->model, 400, 0, 0, 0, 0, 0, NULL, "blocked", 0.0);
            return FILTER_STOP;
        }
        if (gr_res == GUARDRAILS_MASKED && q->sanitized_body != NULL) {
            q->eff_body = q->sanitized_body;
            snprintf(q->guardrail_act, sizeof q->guardrail_act, "masked");
        }

        /* L2: External Webhook Moderation (Inbound) */
        char*       wh_sanitized = NULL;
        size_t      wh_san_len = 0;
        char        wh_reason[128] = {0};
        const char* current_body =
            (q->sanitized_body != NULL) ? q->sanitized_body : (const char*)q->rq->body;
        size_t current_len = (q->sanitized_body != NULL) ? q->sanitized_len : q->rq->body_len;

        guardrails_action_t wh_res = guardrails_inspect_webhook_inbound(q->ac->gr,
                                                                        q->model,
                                                                        q->krec.key_id,
                                                                        current_body,
                                                                        current_len,
                                                                        &wh_sanitized,
                                                                        &wh_san_len,
                                                                        wh_reason,
                                                                        sizeof wh_reason);
        if (wh_res == GUARDRAILS_BLOCKED) {
            char block_msg[256];
            snprintf(block_msg,
                     sizeof block_msg,
                     "Blocked by external moderation webhook: %s",
                     wh_reason[0] ? wh_reason : "content_policy_violation");
            aigate_write_error(q->rc, 400, "content_policy_violation", block_msg);
            record_usage_and_event(
                q->ac, q->krec.key_id, q->model, 400, 0, 0, 0, 0, 0, NULL, "blocked", 0.0);
            return FILTER_STOP;
        }
        if (wh_res == GUARDRAILS_MASKED && wh_sanitized != NULL) {
            if (q->sanitized_body != NULL) {
                free(q->sanitized_body);
            }
            q->sanitized_body = wh_sanitized;
            q->sanitized_len = wh_san_len;
            q->eff_body = wh_sanitized;
            snprintf(q->guardrail_act, sizeof q->guardrail_act, "masked");
        }
    }
    q->eff_len = (q->sanitized_body != NULL) ? q->sanitized_len : q->rq->body_len;
    return FILTER_CONTINUE;
}

/**
 * @brief Inbound Filter 2: Dynamic Prompt Template Injection.
 */
static filter_action_t
filter_prompt_template(chat_req_t* q)
{
    const char*          sys_tmpl = NULL;
    prompt_inject_mode_t mode = PROMPT_MODE_PREPEND;

    /* Priority 1: API Key template */
    if (q->krec.system_prompt[0] != '\0') {
        sys_tmpl = q->krec.system_prompt;
        mode = (prompt_inject_mode_t)q->krec.prompt_mode;
    } else if (q->route.system_prompt[0] != '\0') {
        /* Priority 2: Model route template */
        sys_tmpl = q->route.system_prompt;
        mode = (prompt_inject_mode_t)q->route.prompt_mode;
    }

    if (sys_tmpl == NULL || sys_tmpl[0] == '\0') {
        return FILTER_CONTINUE;
    }

    if (q->jbody == NULL) {
        return FILTER_CONTINUE;
    }

    prompt_template_t tmpl = {
        .system_template = sys_tmpl,
        .mode = mode,
        .prefix_user_prompt = NULL,
        .suffix_user_prompt = NULL,
    };

    char*  mod_json = NULL;
    size_t mod_len = 0;
    int    rc = prompt_template_apply(&tmpl, q->model, q->krec.name, q->jbody, &mod_json, &mod_len);
    if (rc == 0 && mod_json != NULL) {
        if (q->sanitized_body != NULL) {
            free(q->sanitized_body);
        }
        q->sanitized_body = mod_json;
        q->sanitized_len = mod_len;
        q->eff_body = mod_json;
        q->eff_len = mod_len;
    }

    return FILTER_CONTINUE;
}

filter_action_t
filter_chain_execute_inbound(chat_req_t* q)
{
    if (q == NULL) {
        return FILTER_CONTINUE;
    }

    if (filter_guardrails(q) == FILTER_STOP) {
        return FILTER_STOP;
    }

    if (filter_prompt_template(q) == FILTER_STOP) {
        return FILTER_STOP;
    }

    return FILTER_CONTINUE;
}

filter_action_t
filter_chain_execute_outbound(
    chat_req_t* q, const char* resp_body, size_t resp_len, char** out_body, size_t* out_len)
{
    if (out_body != NULL) {
        *out_body = NULL;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (q == NULL || resp_body == NULL || resp_len == 0) {
        return FILTER_CONTINUE;
    }

    if (q->krec.guardrails_enabled && q->ac->gr != NULL) {
        char                wh_reason[128] = {0};
        char*               wh_sanitized = NULL;
        size_t              wh_san_len = 0;
        guardrails_action_t act = guardrails_inspect_webhook_outbound(q->ac->gr,
                                                                      q->model,
                                                                      q->krec.key_id,
                                                                      resp_body,
                                                                      resp_len,
                                                                      &wh_sanitized,
                                                                      &wh_san_len,
                                                                      wh_reason,
                                                                      sizeof wh_reason);
        if (act == GUARDRAILS_BLOCKED) {
            char block_msg[256];
            snprintf(block_msg,
                     sizeof block_msg,
                     "Response blocked by external moderation webhook: %s",
                     wh_reason[0] ? wh_reason : "prohibited_content");
            aigate_write_error(q->rc, 400, "content_policy_violation", block_msg);
            record_usage_and_event(
                q->ac, q->krec.key_id, q->model, 400, 0, 0, 0, 0, 0, NULL, "blocked", 0.0);
            return FILTER_STOP;
        }
        if (act == GUARDRAILS_MASKED && wh_sanitized != NULL) {
            if (out_body != NULL) {
                *out_body = wh_sanitized;
            } else {
                free(wh_sanitized);
            }
            if (out_len != NULL) {
                *out_len = wh_san_len;
            }
            snprintf(q->guardrail_act, sizeof q->guardrail_act, "masked");
            return FILTER_CONTINUE;
        }
    }

    /* De-anonymize session PII tokens back to original values */
    if (q->pii_map.count > 0) {
        const char* cur_body = (out_body != NULL && *out_body != NULL) ? *out_body : resp_body;
        size_t      cur_len = (out_len != NULL && *out_len > 0) ? *out_len : resp_len;
        int         p_changed = 0;
        char* restored = guardrails_restore_pii_text(cur_body, cur_len, &q->pii_map, &p_changed);
        if (p_changed && restored != NULL) {
            if (out_body != NULL && *out_body != NULL) {
                free(*out_body);
            }
            if (out_body != NULL) {
                *out_body = restored;
            } else {
                free(restored);
            }
            if (out_len != NULL) {
                *out_len = strlen(restored);
            }
        }
    }

    return FILTER_CONTINUE;
}
