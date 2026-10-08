/** @file audit_logger.c
 *  @brief Audit log streaming, compliance data models, and dual-channel pipeline.
 */
#define _POSIX_C_SOURCE 200809L
#include "audit_logger.h"
#include <errno.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* --- High-Performance Non-blocking Ring Buffer --- */

struct audit_ring {
    audit_event_t*  slots;
    size_t          head;
    size_t          tail;
    size_t          count;
    size_t          capacity;
    uint64_t        dropped_count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
};

audit_ring_t*
audit_ring_create(size_t capacity)
{
    if (capacity < 4) {
        capacity = 4;
    }
    audit_ring_t* ring = calloc(1, sizeof(*ring));
    if (ring == NULL) {
        return NULL;
    }
    ring->slots = calloc(capacity, sizeof(audit_event_t));
    if (ring->slots == NULL) {
        free(ring);
        return NULL;
    }
    ring->capacity = capacity;
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
    ring->dropped_count = 0;
    pthread_mutex_init(&ring->lock, NULL);
    pthread_cond_init(&ring->not_empty, NULL);
    return ring;
}

void
audit_ring_destroy(audit_ring_t* ring)
{
    if (ring == NULL) {
        return;
    }
    pthread_mutex_lock(&ring->lock);
    for (size_t i = 0; i < ring->count; i++) {
        size_t idx = (ring->head + i) % ring->capacity;
        audit_event_cleanup(&ring->slots[idx]);
    }
    pthread_mutex_unlock(&ring->lock);
    pthread_mutex_destroy(&ring->lock);
    pthread_cond_destroy(&ring->not_empty);
    free(ring->slots);
    free(ring);
}

bool
audit_ring_push(audit_ring_t* ring, const audit_event_t* ev)
{
    if (ring == NULL || ev == NULL) {
        return false;
    }

    pthread_mutex_lock(&ring->lock);
    if (ring->count == ring->capacity) {
        /* Ring is full: evict oldest element at head without blocking */
        audit_event_cleanup(&ring->slots[ring->head]);
        if (audit_event_copy(&ring->slots[ring->head], ev) != 0) {
            pthread_mutex_unlock(&ring->lock);
            return false;
        }
        ring->head = (ring->head + 1) % ring->capacity;
        ring->tail = ring->head;
        ring->dropped_count++;
    } else {
        if (audit_event_copy(&ring->slots[ring->tail], ev) != 0) {
            pthread_mutex_unlock(&ring->lock);
            return false;
        }
        ring->tail = (ring->tail + 1) % ring->capacity;
        ring->count++;
    }

    pthread_cond_signal(&ring->not_empty);
    pthread_mutex_unlock(&ring->lock);
    return true;
}

size_t
audit_ring_pop_batch(audit_ring_t*  ring,
                     audit_event_t* out_batch,
                     size_t         max_count,
                     uint32_t       timeout_ms)
{
    if (ring == NULL || out_batch == NULL || max_count == 0) {
        return 0;
    }

    pthread_mutex_lock(&ring->lock);
    if (ring->count == 0 && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
        ts.tv_sec += (time_t)(nsec / 1000000000ULL);
        ts.tv_nsec = (long)(nsec % 1000000000ULL);

        while (ring->count == 0) {
            int rc = pthread_cond_timedwait(&ring->not_empty, &ring->lock, &ts);
            if (rc != 0) {
                break;
            }
        }
    }

    size_t popped = 0;
    while (popped < max_count && ring->count > 0) {
        out_batch[popped] = ring->slots[ring->head];
        /* Zero slot in ring so ownership is cleanly moved to caller */
        memset(&ring->slots[ring->head], 0, sizeof(audit_event_t));
        ring->head = (ring->head + 1) % ring->capacity;
        ring->count--;
        popped++;
    }

    pthread_mutex_unlock(&ring->lock);
    return popped;
}

size_t
audit_ring_count(audit_ring_t* ring)
{
    if (ring == NULL) {
        return 0;
    }
    pthread_mutex_lock(&ring->lock);
    size_t count = ring->count;
    pthread_mutex_unlock(&ring->lock);
    return count;
}

uint64_t
audit_ring_dropped(audit_ring_t* ring)
{
    if (ring == NULL) {
        return 0;
    }
    pthread_mutex_lock(&ring->lock);
    uint64_t dropped = ring->dropped_count;
    pthread_mutex_unlock(&ring->lock);
    return dropped;
}

/* --- Dual-Channel Audit Logger Engine --- */

struct audit_logger {
    audit_config_t  cfg;
    audit_ring_t*   file_ring;
    audit_ring_t*   webhook_ring;
    pthread_t       file_th;
    bool            file_th_started;
    pthread_t       webhook_th;
    bool            webhook_th_started;
    volatile bool   running;
    volatile bool   reload_requested;
    FILE*           file_fp;
    size_t          current_file_size;
    pthread_mutex_t fp_lock;
    uint64_t        webhook_success_total;
    uint64_t        webhook_failures_total;
    pthread_mutex_t metrics_lock;
};

static void
audit_file_rotate(audit_logger_t* al)
{
    if (al->file_fp != NULL) {
        fclose(al->file_fp);
        al->file_fp = NULL;
    }
    if (al->cfg.max_backups > 0) {
        char old_path[600];
        snprintf(old_path, sizeof(old_path), "%s.%d", al->cfg.log_file, al->cfg.max_backups);
        unlink(old_path);

        for (int i = al->cfg.max_backups - 1; i >= 1; i--) {
            char src[600], dst[600];
            snprintf(src, sizeof(src), "%s.%d", al->cfg.log_file, i);
            snprintf(dst, sizeof(dst), "%s.%d", al->cfg.log_file, i + 1);
            rename(src, dst);
        }

        char first_backup[600];
        snprintf(first_backup, sizeof(first_backup), "%s.1", al->cfg.log_file);
        rename(al->cfg.log_file, first_backup);
    }
    al->file_fp = fopen(al->cfg.log_file, "a");
    al->current_file_size = 0;
}

static void*
audit_file_worker_thread(void* arg)
{
    audit_logger_t* al = (audit_logger_t*)arg;
    audit_event_t   batch[64];

    while (al->running || (al->file_ring != NULL && audit_ring_count(al->file_ring) > 0)) {
        if (al->reload_requested) {
            al->reload_requested = false;
            pthread_mutex_lock(&al->fp_lock);
            if (al->file_fp != NULL) {
                fclose(al->file_fp);
                al->file_fp = NULL;
            }
            if (al->cfg.log_file[0] != '\0') {
                al->file_fp = fopen(al->cfg.log_file, "a");
                if (al->file_fp != NULL) {
                    fseek(al->file_fp, 0, SEEK_END);
                    al->current_file_size = (size_t)ftell(al->file_fp);
                }
            }
            pthread_mutex_unlock(&al->fp_lock);
        }

        size_t count = audit_ring_pop_batch(al->file_ring, batch, 64, 200);
        if (count == 0) {
            continue;
        }

        pthread_mutex_lock(&al->fp_lock);
        if (al->file_fp == NULL && al->cfg.log_file[0] != '\0') {
            al->file_fp = fopen(al->cfg.log_file, "a");
            if (al->file_fp != NULL) {
                fseek(al->file_fp, 0, SEEK_END);
                al->current_file_size = (size_t)ftell(al->file_fp);
            }
        }

        if (al->file_fp != NULL) {
            for (size_t i = 0; i < count; i++) {
                char* line = audit_event_to_ndjson(&batch[i]);
                if (line != NULL) {
                    size_t len = strlen(line);
                    fwrite(line, 1, len, al->file_fp);
                    fputc('\n', al->file_fp);
                    al->current_file_size += len + 1;
                    free(line);
                }
                audit_event_cleanup(&batch[i]);
            }
            fflush(al->file_fp);

            if (al->cfg.max_size_mb > 0 &&
                al->current_file_size >= (size_t)al->cfg.max_size_mb * 1024 * 1024) {
                audit_file_rotate(al);
            }
        } else {
            for (size_t i = 0; i < count; i++) {
                audit_event_cleanup(&batch[i]);
            }
        }
        pthread_mutex_unlock(&al->fp_lock);
    }
    return NULL;
}

static void*
audit_webhook_worker_thread(void* arg)
{
    audit_logger_t* al = (audit_logger_t*)arg;
    audit_event_t   batch[16];

    while (al->running || (al->webhook_ring != NULL && audit_ring_count(al->webhook_ring) > 0)) {
        size_t count = audit_ring_pop_batch(al->webhook_ring, batch, 16, 200);
        for (size_t i = 0; i < count; i++) {
            audit_event_cleanup(&batch[i]);
        }
    }
    return NULL;
}

audit_logger_t*
audit_logger_create(const audit_config_t* cfg)
{
    if (cfg == NULL) {
        return NULL;
    }
    audit_logger_t* al = calloc(1, sizeof(*al));
    if (al == NULL) {
        return NULL;
    }
    al->cfg = *cfg;
    if (al->cfg.max_size_mb <= 0) {
        al->cfg.max_size_mb = 100;
    }
    if (al->cfg.max_backups <= 0) {
        al->cfg.max_backups = 5;
    }
    if (al->cfg.max_prompt_len <= 0) {
        al->cfg.max_prompt_len = 4096;
    }
    if (al->cfg.sample_rate <= 0.0 && cfg->sample_rate <= 0.0) {
        al->cfg.sample_rate = 1.0;
    }

    if (al->cfg.log_file[0] != '\0') {
        al->file_ring = audit_ring_create(4096);
        if (al->file_ring == NULL) {
            free(al);
            return NULL;
        }
    }

    if (al->cfg.webhook_url[0] != '\0') {
        al->webhook_ring = audit_ring_create(1024);
        if (al->webhook_ring == NULL) {
            if (al->file_ring != NULL) {
                audit_ring_destroy(al->file_ring);
            }
            free(al);
            return NULL;
        }
    }

    pthread_mutex_init(&al->fp_lock, NULL);
    pthread_mutex_init(&al->metrics_lock, NULL);
    return al;
}

int
audit_logger_start(audit_logger_t* al)
{
    if (al == NULL) {
        return -1;
    }
    al->running = true;
    if (al->file_ring != NULL) {
        if (pthread_create(&al->file_th, NULL, audit_file_worker_thread, al) != 0) {
            al->running = false;
            return -1;
        }
        al->file_th_started = true;
    }
    if (al->webhook_ring != NULL) {
        if (pthread_create(&al->webhook_th, NULL, audit_webhook_worker_thread, al) != 0) {
            /* non-fatal; continue with file worker */
        } else {
            al->webhook_th_started = true;
        }
    }
    return 0;
}

void
audit_logger_stop(audit_logger_t* al)
{
    if (al == NULL || !al->running) {
        return;
    }
    al->running = false;
    if (al->file_th_started) {
        pthread_join(al->file_th, NULL);
        al->file_th_started = false;
    }
    if (al->webhook_th_started) {
        pthread_join(al->webhook_th, NULL);
        al->webhook_th_started = false;
    }
    pthread_mutex_lock(&al->fp_lock);
    if (al->file_fp != NULL) {
        fflush(al->file_fp);
        fclose(al->file_fp);
        al->file_fp = NULL;
    }
    pthread_mutex_unlock(&al->fp_lock);
}

void
audit_logger_reload(audit_logger_t* al)
{
    if (al == NULL) {
        return;
    }
    al->reload_requested = true;
}

void
audit_logger_record(audit_logger_t* al, const audit_event_t* ev)
{
    if (al == NULL || ev == NULL) {
        return;
    }

    /* Probabilistic sampling for normal INFO events */
    if (ev->severity == AUDIT_SEV_INFO && al->cfg.sample_rate < 1.0) {
        if (al->cfg.sample_rate <= 0.0) {
            return;
        }
        double r = (double)rand() / (double)RAND_MAX;
        if (r > al->cfg.sample_rate) {
            return;
        }
    }

    if (al->file_ring != NULL) {
        audit_ring_push(al->file_ring, ev);
    }
    if (al->webhook_ring != NULL && ev->severity >= AUDIT_SEV_VIOLATION) {
        audit_ring_push(al->webhook_ring, ev);
    }
}

void
audit_logger_destroy(audit_logger_t* al)
{
    if (al == NULL) {
        return;
    }
    audit_logger_stop(al);
    if (al->file_ring != NULL) {
        audit_ring_destroy(al->file_ring);
        al->file_ring = NULL;
    }
    if (al->webhook_ring != NULL) {
        audit_ring_destroy(al->webhook_ring);
        al->webhook_ring = NULL;
    }
    pthread_mutex_destroy(&al->fp_lock);
    pthread_mutex_destroy(&al->metrics_lock);
    free(al);
}

uint64_t
audit_logger_get_dropped_total(audit_logger_t* al)
{
    if (al == NULL) {
        return 0;
    }
    uint64_t total = 0;
    if (al->file_ring != NULL) {
        total += audit_ring_dropped(al->file_ring);
    }
    if (al->webhook_ring != NULL) {
        total += audit_ring_dropped(al->webhook_ring);
    }
    return total;
}

uint64_t
audit_logger_get_webhook_success_total(audit_logger_t* al)
{
    if (al == NULL) {
        return 0;
    }
    pthread_mutex_lock(&al->metrics_lock);
    uint64_t c = al->webhook_success_total;
    pthread_mutex_unlock(&al->metrics_lock);
    return c;
}

uint64_t
audit_logger_get_webhook_failures_total(audit_logger_t* al)
{
    if (al == NULL) {
        return 0;
    }
    pthread_mutex_lock(&al->metrics_lock);
    uint64_t c = al->webhook_failures_total;
    pthread_mutex_unlock(&al->metrics_lock);
    return c;
}
