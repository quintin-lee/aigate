/** @file event_bus.c
 *  @brief Implementation of in-memory pub-sub event bus for real-time telemetry.
 */
#include "event_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void
json_escape_str(const char* in, char* out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < cap; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            out[o++] = '\\';
            out[o++] = in[i];
        } else if ((unsigned char)in[i] < 32) {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

event_bus_t*
event_bus_new(void)
{
    event_bus_t* eb = calloc(1, sizeof(*eb));
    if (eb == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&eb->lock, NULL) != 0) {
        free(eb);
        return NULL;
    }
    eb->next_sub_id = 100;
    return eb;
}

void
event_bus_free(event_bus_t* eb)
{
    if (eb == NULL) {
        return;
    }
    pthread_mutex_lock(&eb->lock);
    eb->destroyed = 1;
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        if (eb->subscribers[i].active) {
            pthread_cond_broadcast(&eb->subscribers[i].cond);
        }
    }
    pthread_mutex_unlock(&eb->lock);

    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        if (eb->subscribers[i].active) {
            eb->subscribers[i].active = 0;
            pthread_cond_destroy(&eb->subscribers[i].cond);
        }
    }
    pthread_mutex_destroy(&eb->lock);
    free(eb);
}

int
event_bus_subscribe(event_bus_t* eb)
{
    if (eb == NULL) {
        return -1;
    }
    pthread_mutex_lock(&eb->lock);
    if (eb->destroyed || eb->n_subscribers >= MAX_EVENT_SUBSCRIBERS) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        if (!eb->subscribers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }

    event_sub_t* sub = &eb->subscribers[slot];
    memset(sub, 0, sizeof(*sub));
    sub->head = 0;
    sub->tail = 0;
    sub->count = 0;
    sub->dropped_count = 0;
    sub->id = ++eb->next_sub_id;
    if (pthread_cond_init(&sub->cond, NULL) != 0) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }
    sub->active = 1;
    eb->n_subscribers++;
    int sub_id = sub->id;
    pthread_mutex_unlock(&eb->lock);
    return sub_id;
}

void
event_bus_unsubscribe(event_bus_t* eb, int sub_id)
{
    if (eb == NULL || sub_id <= 0) {
        return;
    }
    pthread_mutex_lock(&eb->lock);
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        if (eb->subscribers[i].active && eb->subscribers[i].id == sub_id) {
            eb->subscribers[i].active = 0;
            pthread_cond_broadcast(&eb->subscribers[i].cond);
            pthread_cond_destroy(&eb->subscribers[i].cond);
            eb->n_subscribers--;
            break;
        }
    }
    pthread_mutex_unlock(&eb->lock);
}

int
event_bus_pop(event_bus_t* eb, int sub_id, event_item_t* out, int timeout_ms)
{
    if (eb == NULL || sub_id <= 0 || out == NULL) {
        return -1;
    }

    pthread_mutex_lock(&eb->lock);
    event_sub_t* sub = NULL;
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        if (eb->subscribers[i].active && eb->subscribers[i].id == sub_id) {
            sub = &eb->subscribers[i];
            break;
        }
    }
    if (sub == NULL || eb->destroyed) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }

    while (sub->count == 0) {
        if (eb->destroyed || !sub->active) {
            pthread_mutex_unlock(&eb->lock);
            return -1;
        }
        if (timeout_ms <= 0) {
            pthread_mutex_unlock(&eb->lock);
            return 0;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&sub->cond, &eb->lock, &ts);
        if (rc == ETIMEDOUT && sub->count == 0) {
            pthread_mutex_unlock(&eb->lock);
            return 0;
        }
    }

    if (!sub->active || eb->destroyed) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }

    *out = sub->queue[sub->head];
    sub->head = (sub->head + 1) % EVENT_QUEUE_CAPACITY;
    sub->count--;
    pthread_mutex_unlock(&eb->lock);
    return 1;
}

int
event_bus_publish(event_bus_t* eb,
                  event_type_t type,
                  const char*  event_name,
                  const char*  json_payload)
{
    if (eb == NULL) {
        return 0;
    }
    pthread_mutex_lock(&eb->lock);
    if (eb->destroyed) {
        pthread_mutex_unlock(&eb->lock);
        return -1;
    }

    time_t now = time(NULL);
    for (int i = 0; i < MAX_EVENT_SUBSCRIBERS; i++) {
        event_sub_t* sub = &eb->subscribers[i];
        if (!sub->active) {
            continue;
        }

        if (sub->count == EVENT_QUEUE_CAPACITY) {
            /* Queue full: drop oldest event */
            sub->head = (sub->head + 1) % EVENT_QUEUE_CAPACITY;
            sub->count--;
            sub->dropped_count++;
        }

        event_item_t* item = &sub->queue[sub->tail];
        item->type = type;
        item->ts = now;
        snprintf(item->event_name, sizeof item->event_name, "%s", event_name ? event_name : "");
        snprintf(item->payload, sizeof item->payload, "%s", json_payload ? json_payload : "{}");

        sub->tail = (sub->tail + 1) % EVENT_QUEUE_CAPACITY;
        sub->count++;
        pthread_cond_signal(&sub->cond);
    }
    pthread_mutex_unlock(&eb->lock);
    return 0;
}

void
event_bus_publish_request(event_bus_t* eb,
                           long         key_id,
                           const char*  model,
                           const char*  provider,
                           int          status,
                           uint64_t     latency_ns,
                           long         prompt_tokens,
                           long         completion_tokens,
                           double       cost,
                           const char*  guardrail_act)
{
    if (eb == NULL) {
        return;
    }
    char safe_model[128];
    char safe_provider[64];
    char safe_gr[32];
    json_escape_str(model, safe_model, sizeof safe_model);
    json_escape_str(provider, safe_provider, sizeof safe_provider);
    json_escape_str(guardrail_act, safe_gr, sizeof safe_gr);

    long lat_ms = (long)(latency_ns / 1000000ULL);
    char buf[EVENT_MAX_PAYLOAD];
    snprintf(buf,
             sizeof buf,
             "{\"ts\":%ld,\"key_id\":%ld,\"model\":\"%s\",\"provider\":\"%s\",\"status\":%d,"
             "\"latency_ms\":%ld,\"prompt_tokens\":%ld,\"completion_tokens\":%ld,\"cost\":%.6f,"
             "\"guardrail\":\"%s\",\"cached\":%s}",
             (long)time(NULL),
             key_id,
             safe_model,
             safe_provider,
             status,
             lat_ms,
             prompt_tokens,
             completion_tokens,
             cost,
             safe_gr,
             strcmp(safe_provider, "cache") == 0 ? "true" : "false");
    event_bus_publish(eb, EVENT_REQUEST, "request", buf);
}

void
event_bus_publish_cb(event_bus_t* eb,
                     const char*  provider,
                     const char*  model,
                     const char*  old_state,
                     const char*  new_state,
                     const char*  reason)
{
    if (eb == NULL) {
        return;
    }
    char safe_provider[64], safe_model[128], safe_reason[256];
    json_escape_str(provider, safe_provider, sizeof safe_provider);
    json_escape_str(model, safe_model, sizeof safe_model);
    json_escape_str(reason, safe_reason, sizeof safe_reason);

    char buf[EVENT_MAX_PAYLOAD];
    snprintf(buf,
             sizeof buf,
             "{\"ts\":%ld,\"provider\":\"%s\",\"model\":\"%s\",\"old_state\":\"%s\","
             "\"new_state\":\"%s\",\"reason\":\"%s\"}",
             (long)time(NULL),
             safe_provider,
             safe_model,
             old_state ? old_state : "",
             new_state ? new_state : "",
             safe_reason);
    event_bus_publish(eb, EVENT_CIRCUIT_BREAKER, "circuit_breaker", buf);
}

void
event_bus_publish_health(event_bus_t* eb,
                         const char*  provider,
                         const char*  status,
                         long         latency_ms,
                         int          http_status,
                         const char*  error)
{
    if (eb == NULL) {
        return;
    }
    char safe_provider[64], safe_err[256];
    json_escape_str(provider, safe_provider, sizeof safe_provider);
    json_escape_str(error, safe_err, sizeof safe_err);

    char buf[EVENT_MAX_PAYLOAD];
    snprintf(buf,
             sizeof buf,
             "{\"ts\":%ld,\"provider\":\"%s\",\"status\":\"%s\",\"latency_ms\":%ld,"
             "\"http_status\":%d,\"error\":\"%s\"}",
             (long)time(NULL),
             safe_provider,
             status ? status : "UNKNOWN",
             latency_ms,
             http_status,
             safe_err);
    event_bus_publish(eb, EVENT_HEALTH_PROBE, "health_probe", buf);
}

void
event_bus_publish_budget(event_bus_t* eb,
                         const char*  type,
                         long         id,
                         const char*  name,
                         double       percent,
                         double       current_usd,
                         double       budget_usd)
{
    if (eb == NULL) {
        return;
    }
    char safe_type[32], safe_name[128];
    json_escape_str(type, safe_type, sizeof safe_type);
    json_escape_str(name, safe_name, sizeof safe_name);

    char buf[EVENT_MAX_PAYLOAD];
    snprintf(buf,
             sizeof buf,
             "{\"ts\":%ld,\"type\":\"%s\",\"id\":%ld,\"name\":\"%s\",\"percent\":%.1f,"
             "\"current_usd\":%.4f,\"budget_usd\":%.4f}",
             (long)time(NULL),
             safe_type,
             id,
             safe_name,
             percent,
             current_usd,
             budget_usd);
    event_bus_publish(eb, EVENT_BUDGET_ALERT, "budget_alert", buf);
}

void
event_bus_publish_ping(event_bus_t* eb)
{
    if (eb == NULL) {
        return;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "{\"ts\":%ld}", (long)time(NULL));
    event_bus_publish(eb, EVENT_PING, "ping", buf);
}
