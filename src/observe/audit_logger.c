/** @file audit_logger.c
 *  @brief Audit log streaming, compliance data models, and dual-channel pipeline.
 */
#define _POSIX_C_SOURCE 200809L
#include "audit_logger.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char*
severity_to_str(audit_severity_t sev)
{
    switch (sev) {
    case AUDIT_SEV_INFO:
        return "INFO";
    case AUDIT_SEV_WARN:
        return "WARN";
    case AUDIT_SEV_VIOLATION:
        return "VIOLATION";
    case AUDIT_SEV_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

void
audit_event_init(audit_event_t* ev)
{
    if (ev == NULL) {
        return;
    }
    memset(ev, 0, sizeof(*ev));
}

void
audit_event_cleanup(audit_event_t* ev)
{
    if (ev == NULL) {
        return;
    }
    if (ev->prompt_snapshot != NULL) {
        free(ev->prompt_snapshot);
        ev->prompt_snapshot = NULL;
    }
    ev->prompt_snapshot_len = 0;
}

int
audit_event_copy(audit_event_t* dst, const audit_event_t* src)
{
    if (dst == NULL || src == NULL) {
        return -1;
    }
    audit_event_cleanup(dst);
    *dst = *src;
    dst->prompt_snapshot = NULL;
    dst->prompt_snapshot_len = 0;

    if (src->prompt_snapshot != NULL && src->prompt_snapshot_len > 0) {
        dst->prompt_snapshot = malloc(src->prompt_snapshot_len + 1);
        if (dst->prompt_snapshot == NULL) {
            return -1;
        }
        memcpy(dst->prompt_snapshot, src->prompt_snapshot, src->prompt_snapshot_len);
        dst->prompt_snapshot[src->prompt_snapshot_len] = '\0';
        dst->prompt_snapshot_len = src->prompt_snapshot_len;
    }
    return 0;
}

int
audit_event_set_prompt(audit_event_t* ev, const char* prompt, size_t max_len)
{
    if (ev == NULL) {
        return -1;
    }
    if (ev->prompt_snapshot != NULL) {
        free(ev->prompt_snapshot);
        ev->prompt_snapshot = NULL;
        ev->prompt_snapshot_len = 0;
    }
    if (prompt == NULL || max_len == 0) {
        return 0;
    }

    size_t in_len = strlen(prompt);
    size_t copy_len = (in_len > max_len) ? max_len : in_len;

    ev->prompt_snapshot = malloc(copy_len + 1);
    if (ev->prompt_snapshot == NULL) {
        return -1;
    }
    memcpy(ev->prompt_snapshot, prompt, copy_len);
    ev->prompt_snapshot[copy_len] = '\0';
    ev->prompt_snapshot_len = copy_len;
    return 0;
}

static void
format_iso8601_ms(int64_t timestamp_ms, char* out_buf, size_t cap)
{
    time_t sec = (time_t)(timestamp_ms / 1000LL);
    int    ms = (int)(timestamp_ms % 1000LL);
    if (ms < 0) {
        ms = 0;
    }
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);
    snprintf(out_buf,
             cap,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_buf.tm_year + 1900,
             tm_buf.tm_mon + 1,
             tm_buf.tm_mday,
             tm_buf.tm_hour,
             tm_buf.tm_min,
             tm_buf.tm_sec,
             ms);
}

char*
audit_event_to_ndjson(const audit_event_t* ev)
{
    if (ev == NULL) {
        return NULL;
    }

    char ts_buf[32];
    format_iso8601_ms(ev->timestamp_ms, ts_buf, sizeof(ts_buf));

    json_t* root = json_object();
    json_object_set_new(root, "ts", json_string(ts_buf));
    json_object_set_new(root, "trace_id", json_string(ev->trace_id));
    json_object_set_new(root, "severity", json_string(severity_to_str(ev->severity)));
    json_object_set_new(root, "key_id", json_integer(ev->key_id));
    json_object_set_new(root, "client_ip", json_string(ev->client_ip));
    json_object_set_new(root, "model", json_string(ev->model));
    json_object_set_new(root, "provider", json_string(ev->provider));
    json_object_set_new(root, "status", json_integer(ev->http_status));
    json_object_set_new(root, "prompt_tokens", json_integer(ev->prompt_tokens));
    json_object_set_new(root, "completion_tokens", json_integer(ev->completion_tokens));
    json_object_set_new(root, "latency_us", json_integer((json_int_t)(ev->latency_ns / 1000ULL)));
    if (ev->ttft_ns > 0) {
        json_object_set_new(root, "ttft_us", json_integer((json_int_t)(ev->ttft_ns / 1000ULL)));
    }

    if (ev->violation_type[0] != '\0' || ev->rule_detail[0] != '\0') {
        json_t* v_obj = json_object();
        json_object_set_new(v_obj, "type", json_string(ev->violation_type));
        json_object_set_new(v_obj, "detail", json_string(ev->rule_detail));
        json_object_set_new(root, "violation", v_obj);
    }

    if (ev->prompt_snapshot != NULL) {
        json_object_set_new(root, "prompt", json_string(ev->prompt_snapshot));
    }

    char* out = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return out;
}

char*
audit_event_to_webhook_payload(const audit_event_t* ev, audit_webhook_format_t fmt)
{
    if (ev == NULL) {
        return NULL;
    }

    char ts_buf[32];
    format_iso8601_ms(ev->timestamp_ms, ts_buf, sizeof(ts_buf));
    const char* sev_str = severity_to_str(ev->severity);
    const char* prompt_str = (ev->prompt_snapshot != NULL) ? ev->prompt_snapshot : "";

    if (fmt == AUDIT_HOOK_FEISHU) {
        json_t* root = json_object();
        json_object_set_new(root, "msg_type", json_string("interactive"));

        json_t* card = json_object();
        json_t* header = json_object();
        json_t* title = json_object();
        json_object_set_new(title, "tag", json_string("plain_text"));
        json_object_set_new(title, "content", json_string("🚨 aigate 安全审计告警"));
        json_object_set_new(header, "title", title);
        json_object_set_new(header,
                            "template",
                            json_string(ev->severity >= AUDIT_SEV_VIOLATION ? "red" : "orange"));
        json_object_set_new(card, "header", header);

        json_t* elements = json_array();

        /* Fields div */
        json_t* div_fields = json_object();
        json_object_set_new(div_fields, "tag", json_string("div"));
        json_t* fields = json_array();

        char f1[128], f2[128], f3[128], f4[128];
        snprintf(f1, sizeof(f1), "**级别:** %s", sev_str);
        snprintf(f2, sizeof(f2), "**状态码:** %d", ev->http_status);
        snprintf(f3, sizeof(f3), "**模型:** %s", ev->model);
        snprintf(f4, sizeof(f4), "**客户端 IP:** %s", ev->client_ip);

        const char* field_texts[4] = {f1, f2, f3, f4};
        for (int i = 0; i < 4; i++) {
            json_t* f = json_object();
            json_object_set_new(f, "is_short", json_true());
            json_t* t = json_object();
            json_object_set_new(t, "tag", json_string("lark_md"));
            json_object_set_new(t, "content", json_string(field_texts[i]));
            json_object_set_new(f, "text", t);
            json_array_append_new(fields, f);
        }
        json_object_set_new(div_fields, "fields", fields);
        json_array_append_new(elements, div_fields);

        /* Detail div */
        json_t* div_detail = json_object();
        json_object_set_new(div_detail, "tag", json_string("div"));
        json_t* text_obj = json_object();
        json_object_set_new(text_obj, "tag", json_string("lark_md"));

        char detail_buf[6144];
        snprintf(detail_buf,
                 sizeof(detail_buf),
                 "**Trace ID:** `%s`\n**违规类型:** %s\n**命中规则:** %s\n**Prompt "
                 "现场:**\n```\n%s\n```",
                 ev->trace_id,
                 ev->violation_type,
                 ev->rule_detail,
                 prompt_str);
        json_object_set_new(text_obj, "content", json_string(detail_buf));
        json_object_set_new(div_detail, "text", text_obj);
        json_array_append_new(elements, div_detail);

        json_object_set_new(card, "elements", elements);
        json_object_set_new(root, "card", card);

        char* res = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return res;
    }

    if (fmt == AUDIT_HOOK_DINGTALK) {
        json_t* root = json_object();
        json_object_set_new(root, "msgtype", json_string("markdown"));
        json_t* md = json_object();
        json_object_set_new(md, "title", json_string("🚨 aigate 安全审计告警"));

        char text_buf[6144];
        snprintf(text_buf,
                 sizeof(text_buf),
                 "### 🚨 aigate 安全审计告警\n"
                 "- **时间:** %s\n"
                 "- **级别:** %s\n"
                 "- **模型:** %s\n"
                 "- **客户端 IP:** %s\n"
                 "- **状态码:** %d\n"
                 "- **违规类型:** %s\n"
                 "- **命中规则:** %s\n"
                 "- **Trace ID:** `%s`\n"
                 "**Prompt 现场:**\n> %s\n",
                 ts_buf,
                 sev_str,
                 ev->model,
                 ev->client_ip,
                 ev->http_status,
                 ev->violation_type,
                 ev->rule_detail,
                 ev->trace_id,
                 prompt_str);
        json_object_set_new(md, "text", json_string(text_buf));
        json_object_set_new(root, "markdown", md);

        char* res = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return res;
    }

    if (fmt == AUDIT_HOOK_WECHAT_WORK) {
        json_t* root = json_object();
        json_object_set_new(root, "msgtype", json_string("markdown"));
        json_t* md = json_object();

        char text_buf[6144];
        snprintf(text_buf,
                 sizeof(text_buf),
                 "### <font color=\"warning\">🚨 aigate 安全审计告警</font>\n"
                 "> 时间: <font color=\"comment\">%s</font>\n"
                 "> 级别: <font color=\"comment\">%s</font>\n"
                 "> 模型: %s\n"
                 "> 客户端 IP: %s\n"
                 "> 状态码: %d\n"
                 "> 违规类型: %s\n"
                 "> 命中规则: %s\n"
                 "> Trace ID: `%s`\n"
                 "> Prompt 现场: %s\n",
                 ts_buf,
                 sev_str,
                 ev->model,
                 ev->client_ip,
                 ev->http_status,
                 ev->violation_type,
                 ev->rule_detail,
                 ev->trace_id,
                 prompt_str);
        json_object_set_new(md, "content", json_string(text_buf));
        json_object_set_new(root, "markdown", md);

        char* res = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return res;
    }

    /* Default: Standard SIEM format */
    json_t* root = json_object();
    json_object_set_new(root, "event", json_string("audit_alert"));
    json_object_set_new(root, "timestamp", json_string(ts_buf));
    json_object_set_new(root, "severity", json_string(sev_str));
    json_object_set_new(root, "trace_id", json_string(ev->trace_id));
    json_object_set_new(root, "key_id", json_integer(ev->key_id));
    json_object_set_new(root, "client_ip", json_string(ev->client_ip));
    json_object_set_new(root, "model", json_string(ev->model));
    json_object_set_new(root, "provider", json_string(ev->provider));
    json_object_set_new(root, "status", json_integer(ev->http_status));

    json_t* v_obj = json_object();
    json_object_set_new(v_obj, "type", json_string(ev->violation_type));
    json_object_set_new(v_obj, "detail", json_string(ev->rule_detail));
    json_object_set_new(root, "violation", v_obj);

    if (ev->prompt_snapshot != NULL) {
        json_object_set_new(root, "prompt", json_string(ev->prompt_snapshot));
    }

    char* res = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return res;
}
