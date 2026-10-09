#include "filter_chain.h"
#include "guardrails.h"
#include "prompt_template.h"
#include "jailbreak_detector.h"
#include "watermark_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
            aigate_record_audit(q->ac,
                                q->trace_ctx.trace_id,
                                (q->rq != NULL) ? q->rq->client_ip : NULL,
                                q->krec.key_id,
                                q->model,
                                NULL,
                                400,
                                0,
                                0,
                                0,
                                0,
                                AUDIT_SEV_VIOLATION,
                                "guardrail_block",
                                matched_rule[0] ? matched_rule : "blocked content",
                                (q->rq != NULL) ? (const char*)q->rq->body : NULL,
                                (q->rq != NULL) ? q->rq->body_len : 0);
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
            aigate_record_audit(q->ac,
                                q->trace_ctx.trace_id,
                                (q->rq != NULL) ? q->rq->client_ip : NULL,
                                q->krec.key_id,
                                q->model,
                                NULL,
                                400,
                                0,
                                0,
                                0,
                                0,
                                AUDIT_SEV_VIOLATION,
                                "guardrail_block",
                                wh_reason[0] ? wh_reason : "content_policy_violation",
                                (q->rq != NULL) ? (const char*)q->rq->body : NULL,
                                (q->rq != NULL) ? q->rq->body_len : 0);
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

/**
 * @brief Inbound Filter 2: Heuristic jailbreak & adversarial prompt injection defense.
 */
static filter_action_t
filter_jailbreak(chat_req_t* q)
{
    if (q == NULL || !q->krec.guardrails_enabled || q->eff_body == NULL || q->eff_len == 0) {
        return FILTER_CONTINUE;
    }

    jailbreak_result_t res;
    memset(&res, 0, sizeof(res));
    jailbreak_action_t act =
        jailbreak_detector_inspect(NULL, (const char*)q->eff_body, q->eff_len, &res);

    if (act == JAILBREAK_ACTION_BLOCK) {
        char block_msg[256];
        snprintf(block_msg,
                 sizeof block_msg,
                 "Blocked by safety guardrail: adversarial injection detected (%s)",
                 res.rule_tag[0] ? res.rule_tag : "jailbreak");
        aigate_write_error(q->rc, 400, "adversarial_injection_detected", block_msg);
        if (q->ac != NULL) {
            record_usage_and_event(
                q->ac, q->krec.key_id, q->model, 400, 0, 0, 0, 0, 0, NULL, "blocked", 0.0);
            aigate_record_audit(q->ac,
                                q->trace_ctx.trace_id,
                                (q->rq != NULL) ? q->rq->client_ip : NULL,
                                q->krec.key_id,
                                q->model,
                                NULL,
                                400,
                                0,
                                0,
                                0,
                                0,
                                AUDIT_SEV_VIOLATION,
                                "jailbreak_detected",
                                res.reason[0] ? res.reason : "adversarial prompt injection",
                                (q->rq != NULL) ? (const char*)q->rq->body : NULL,
                                (q->rq != NULL) ? q->rq->body_len : 0);
        }
        return FILTER_STOP;
    }

    if (act == JAILBREAK_ACTION_FLAG) {
        snprintf(q->guardrail_act, sizeof q->guardrail_act, "flagged");
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

    if (filter_jailbreak(q) == FILTER_STOP) {
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
            aigate_record_audit(q->ac,
                                q->trace_ctx.trace_id,
                                (q->rq != NULL) ? q->rq->client_ip : NULL,
                                q->krec.key_id,
                                q->model,
                                NULL,
                                400,
                                0,
                                0,
                                0,
                                0,
                                AUDIT_SEV_VIOLATION,
                                "guardrail_block_outbound",
                                wh_reason[0] ? wh_reason : "prohibited_content",
                                resp_body,
                                resp_len);
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

    /* Watermark steganographic injection if enabled on the API key */
    if (q->krec.watermark_enabled) {
        const char* cur_body = (out_body != NULL && *out_body != NULL) ? *out_body : resp_body;
        json_t*     root = json_loads(cur_body, 0, NULL);
        if (root != NULL && json_is_object(root)) {
            bool                modified = false;
            watermark_payload_t wp;
            memset(&wp, 0, sizeof(wp));
            wp.timestamp = (uint32_t)time(NULL);
            wp.key_id = (uint32_t)q->krec.key_id;
            if (q->trace_ctx.trace_id[0] != '\0') {
                wp.short_trace = strtoull(q->trace_ctx.trace_id, NULL, 16);
                if (wp.short_trace == 0) {
                    for (const char* p = q->trace_ctx.trace_id; *p; p++) {
                        wp.short_trace = (wp.short_trace * 31) + (unsigned char)*p;
                    }
                }
            } else {
                wp.short_trace = ((uint64_t)wp.key_id << 32) ^ (uint64_t)wp.timestamp;
            }

            /* 1. Check OpenAI format: choices[0].message.content */
            json_t* choices = json_object_get(root, "choices");
            if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
                json_t* choice0 = json_array_get(choices, 0);
                if (choice0 != NULL && json_is_object(choice0)) {
                    json_t* msg = json_object_get(choice0, "message");
                    if (msg != NULL && json_is_object(msg)) {
                        json_t* content_val = json_object_get(msg, "content");
                        if (content_val != NULL && json_is_string(content_val)) {
                            const char* raw_txt = json_string_value(content_val);
                            if (raw_txt != NULL && raw_txt[0] != '\0') {
                                size_t wm_sz = 0;
                                char*  wm_txt =
                                    watermark_inject(raw_txt, strlen(raw_txt), &wp, &wm_sz);
                                if (wm_txt != NULL) {
                                    json_object_set_new(msg, "content", json_string(wm_txt));
                                    free(wm_txt);
                                    modified = true;
                                }
                            }
                        }
                    }
                }
            }

            /* 2. Check Anthropic format: content[0].text */
            if (!modified) {
                json_t* content_arr = json_object_get(root, "content");
                if (content_arr != NULL && json_is_array(content_arr) &&
                    json_array_size(content_arr) > 0) {
                    json_t* block0 = json_array_get(content_arr, 0);
                    if (block0 != NULL && json_is_object(block0)) {
                        json_t* text_val = json_object_get(block0, "text");
                        if (text_val != NULL && json_is_string(text_val)) {
                            const char* raw_txt = json_string_value(text_val);
                            if (raw_txt != NULL && raw_txt[0] != '\0') {
                                size_t wm_sz = 0;
                                char*  wm_txt =
                                    watermark_inject(raw_txt, strlen(raw_txt), &wp, &wm_sz);
                                if (wm_txt != NULL) {
                                    json_object_set_new(block0, "text", json_string(wm_txt));
                                    free(wm_txt);
                                    modified = true;
                                }
                            }
                        }
                    }
                }
            }

            if (modified) {
                char* new_json = json_dumps(root, JSON_COMPACT);
                if (new_json != NULL) {
                    if (out_body != NULL && *out_body != NULL) {
                        free(*out_body);
                    }
                    if (out_body != NULL) {
                        *out_body = new_json;
                    } else {
                        free(new_json);
                    }
                    if (out_len != NULL) {
                        *out_len = strlen(new_json);
                    }
                }
            }
            json_decref(root);
        }
    }

    return FILTER_CONTINUE;
}
