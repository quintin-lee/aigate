/** @file health_prober.c
 *  @brief Active upstream health probing engine implementation.
 */
#include "health_prober.h"
#include "provider_adapter.h"
#include "upstream_client.h"
#include "secrets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>

const char*
health_status_str(health_status_t st)
{
    switch (st) {
    case HEALTH_STATUS_HEALTHY:
        return "HEALTHY";
    case HEALTH_STATUS_DEGRADED:
        return "DEGRADED";
    case HEALTH_STATUS_DOWN:
        return "DOWN";
    case HEALTH_STATUS_PAUSED:
        return "PAUSED";
    default:
        return "UNKNOWN";
    }
}

/** @brief 解析探针密钥引用：空串留空；`env:VAR` 读环境；`pg:<blob>` 用 master 解密；否则原文拷贝。
 *  @return 0 成功；环境缺失/无 master/解密失败返回 -1。 */
static int
prober_resolve_key(health_prober_t* hp, const char* key_ref, char* out_key, size_t out_sz)
{
    if (key_ref == NULL || key_ref[0] == '\0') {
        out_key[0] = '\0';
        return 0;
    }
    if (strncmp(key_ref, "env:", 4) == 0) {
        const char* env = getenv(key_ref + 4);
        if (env == NULL || env[0] == '\0') {
            return -1;
        }
        snprintf(out_key, out_sz, "%s", env);
        return 0;
    }
    if (strncmp(key_ref, "pg:", 3) == 0) {
        if (!hp->have_master_key) {
            return -1;
        }
        if (secret_decrypt(hp->master_key, key_ref + 3, out_key, out_sz, NULL) != 0) {
            return -1;
        }
        return 0;
    }
    snprintf(out_key, out_sz, "%s", key_ref);
    return 0;
}

health_prober_t*
health_prober_new(pg_store_t*    ps,
                  const uint8_t* master_key,
                  event_bus_t*   eb,
                  int            interval_sec)
{
    health_prober_t* hp = calloc(1, sizeof(*hp));
    if (hp == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&hp->lock, NULL) != 0) {
        free(hp);
        return NULL;
    }
    if (pthread_cond_init(&hp->cond, NULL) != 0) {
        pthread_mutex_destroy(&hp->lock);
        free(hp);
        return NULL;
    }
    hp->ps = ps;
    if (master_key != NULL) {
        memcpy(hp->master_key, master_key, 32);
        hp->have_master_key = 1;
    }
    hp->eb = eb;
    hp->interval_sec = interval_sec > 0 ? interval_sec : 60;
    return hp;
}

void
health_prober_free(health_prober_t* hp)
{
    if (hp == NULL) {
        return;
    }
    health_prober_stop(hp);
    pthread_mutex_destroy(&hp->lock);
    pthread_cond_destroy(&hp->cond);
    free(hp);
}

/** @brief 后台探针线程：按 interval_sec 周期调用 health_prober_probe_all，stop 后退出。
 *  @return 恒 NULL。 */
static void*
prober_thread_func(void* arg)
{
    health_prober_t* hp = arg;
    while (1) {
        pthread_mutex_lock(&hp->lock);
        if (!hp->running) {
            pthread_mutex_unlock(&hp->lock);
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (hp->interval_sec > 0 ? hp->interval_sec : 60);
        pthread_cond_timedwait(&hp->cond, &hp->lock, &ts);
        if (!hp->running) {
            pthread_mutex_unlock(&hp->lock);
            break;
        }
        pthread_mutex_unlock(&hp->lock);

        health_prober_probe_all(hp);
    }
    return NULL;
}

int
health_prober_start(health_prober_t* hp)
{
    if (hp == NULL) {
        return -1;
    }
    pthread_mutex_lock(&hp->lock);
    if (hp->running || hp->thread_started) {
        pthread_mutex_unlock(&hp->lock);
        return 0;
    }
    hp->running = 1;
    if (pthread_create(&hp->thread, NULL, prober_thread_func, hp) != 0) {
        hp->running = 0;
        pthread_mutex_unlock(&hp->lock);
        return -1;
    }
    hp->thread_started = 1;
    pthread_mutex_unlock(&hp->lock);
    return 0;
}

void
health_prober_stop(health_prober_t* hp)
{
    if (hp == NULL) {
        return;
    }
    pthread_mutex_lock(&hp->lock);
    if (!hp->running && !hp->thread_started) {
        pthread_mutex_unlock(&hp->lock);
        return;
    }
    hp->running = 0;
    pthread_cond_broadcast(&hp->cond);
    pthread_mutex_unlock(&hp->lock);

    if (hp->thread_started) {
        pthread_join(hp->thread, NULL);
        hp->thread_started = 0;
    }
}

void
health_prober_record_result(health_prober_t* hp,
                            long             provider_id,
                            const char*      name,
                            const char*      endpoint,
                            const char*      provider_type,
                            int              http_status,
                            long             latency_ms,
                            int              probe_rc)
{
    if (hp == NULL || name == NULL) {
        return;
    }

    pthread_mutex_lock(&hp->lock);
    provider_health_t* ph = NULL;
    for (int i = 0; i < hp->n_providers; i++) {
        if ((provider_id > 0 && hp->providers[i].id == provider_id) ||
            strcmp(hp->providers[i].provider_name, name) == 0) {
            ph = &hp->providers[i];
            break;
        }
    }
    if (ph == NULL) {
        if (hp->n_providers < MAX_TRACKED_PROVIDERS) {
            ph = &hp->providers[hp->n_providers++];
            memset(ph, 0, sizeof(*ph));
        } else {
            ph = &hp->providers[0]; /* recycle first */
        }
    }

    ph->id = provider_id;
    snprintf(ph->provider_name, sizeof ph->provider_name, "%s", name);
    if (endpoint != NULL) {
        snprintf(ph->endpoint, sizeof ph->endpoint, "%s", endpoint);
    }
    if (provider_type != NULL) {
        snprintf(ph->provider_type, sizeof ph->provider_type, "%s", provider_type);
    }
    ph->last_check_ts = time(NULL);
    ph->last_http_status = http_status;
    ph->latency_ms = latency_ms;

    health_status_t old_status = ph->status;
    health_status_t new_status = HEALTH_STATUS_UNKNOWN;
    char            err_buf[256] = {0};

    if (probe_rc == 0) {
        if (http_status >= 200 && http_status < 400) {
            ph->consecutive_failures = 0;
            ph->consecutive_successes++;
            if (latency_ms >= 2000) {
                new_status = HEALTH_STATUS_DEGRADED;
                snprintf(err_buf, sizeof err_buf, "High latency: %ldms", latency_ms);
            } else {
                new_status = HEALTH_STATUS_HEALTHY;
                err_buf[0] = '\0';
            }
        } else {
            ph->consecutive_successes = 0;
            ph->consecutive_failures++;
            if (http_status == 401 || http_status == 403) {
                new_status = HEALTH_STATUS_DOWN;
                snprintf(err_buf, sizeof err_buf, "HTTP %d: Invalid API key", http_status);
            } else if (http_status == 429) {
                new_status = HEALTH_STATUS_DEGRADED;
                snprintf(err_buf, sizeof err_buf, "HTTP 429: Rate limited");
            } else if (ph->consecutive_failures >= 2) {
                new_status = HEALTH_STATUS_DOWN;
                snprintf(err_buf, sizeof err_buf, "HTTP %d: Server error", http_status);
            } else {
                new_status = HEALTH_STATUS_DEGRADED;
                snprintf(err_buf, sizeof err_buf, "HTTP %d: Transient error", http_status);
            }
        }
    } else if (probe_rc == -110) {
        ph->consecutive_successes = 0;
        ph->consecutive_failures++;
        new_status = (ph->consecutive_failures >= 2) ? HEALTH_STATUS_DOWN : HEALTH_STATUS_DEGRADED;
        snprintf(err_buf, sizeof err_buf, "Connection timed out");
    } else {
        ph->consecutive_successes = 0;
        ph->consecutive_failures++;
        new_status = (ph->consecutive_failures >= 2) ? HEALTH_STATUS_DOWN : HEALTH_STATUS_DEGRADED;
        snprintf(err_buf, sizeof err_buf, "Transport unreachable (rc=%d)", probe_rc);
    }

    ph->status = new_status;
    snprintf(ph->last_error, sizeof ph->last_error, "%s", err_buf);
    pthread_mutex_unlock(&hp->lock);

    if (hp->eb != NULL && (old_status != new_status || probe_rc != 0)) {
        event_bus_publish_health(hp->eb,
                                 name,
                                 health_status_str(new_status),
                                 latency_ms,
                                 http_status,
                                 err_buf);
    }
}

int
health_prober_probe_all(health_prober_t* hp)
{
    if (hp == NULL || hp->ps == NULL) {
        return -1;
    }
    const pg_ops_t* ops = pg_store_ops(hp->ps);
    if (ops == NULL || ops->list_providers == NULL) {
        return -1;
    }

    provider_rec_t* recs = calloc(MAX_TRACKED_PROVIDERS, sizeof(provider_rec_t));
    if (recs == NULL) {
        return -1;
    }
    int n = 0;
    if (ops->list_providers(ops->ctx, recs, MAX_TRACKED_PROVIDERS, &n) != 0) {
        free(recs);
        return -1;
    }

    time_t now = time(NULL);
    for (int i = 0; i < n; i++) {
        provider_rec_t* p = &recs[i];
        if (!p->enabled) {
            pthread_mutex_lock(&hp->lock);
            provider_health_t* ph = NULL;
            for (int k = 0; k < hp->n_providers; k++) {
                if (hp->providers[k].id == p->id) {
                    ph = &hp->providers[k];
                    break;
                }
            }
            if (ph == NULL && hp->n_providers < MAX_TRACKED_PROVIDERS) {
                ph = &hp->providers[hp->n_providers++];
            }
            if (ph != NULL) {
                ph->id = p->id;
                snprintf(ph->provider_name, sizeof ph->provider_name, "%s", p->name);
                snprintf(ph->endpoint, sizeof ph->endpoint, "%s", p->endpoint);
                snprintf(ph->provider_type, sizeof ph->provider_type, "%s", p->provider_type);
                ph->status = HEALTH_STATUS_PAUSED;
                ph->last_check_ts = now;
                snprintf(ph->last_error, sizeof ph->last_error, "Provider disabled");
            }
            pthread_mutex_unlock(&hp->lock);
            provider_rec_free(p);
            continue;
        }

        provider_probe_plan_t plan;
        if (provider_probe_plan(p->provider_type, p->endpoint, &plan) != 0) {
            pthread_mutex_lock(&hp->lock);
            provider_health_t* ph = NULL;
            for (int k = 0; k < hp->n_providers; k++) {
                if (hp->providers[k].id == p->id) {
                    ph = &hp->providers[k];
                    break;
                }
            }
            if (ph == NULL && hp->n_providers < MAX_TRACKED_PROVIDERS) {
                ph = &hp->providers[hp->n_providers++];
            }
            if (ph != NULL) {
                ph->id = p->id;
                snprintf(ph->provider_name, sizeof ph->provider_name, "%s", p->name);
                snprintf(ph->endpoint, sizeof ph->endpoint, "%s", p->endpoint);
                snprintf(ph->provider_type, sizeof ph->provider_type, "%s", p->provider_type);
                ph->status = HEALTH_STATUS_DOWN;
                ph->last_check_ts = now;
                snprintf(ph->last_error, sizeof ph->last_error, "Probe unsupported for type");
            }
            pthread_mutex_unlock(&hp->lock);
            provider_rec_free(p);
            continue;
        }

        char key[1080];
        if (prober_resolve_key(hp, p->api_key, key, sizeof key) != 0) {
            health_prober_record_result(hp,
                                        p->id,
                                        p->name,
                                        p->endpoint,
                                        p->provider_type,
                                        401,
                                        0,
                                        -502);
            provider_rec_free(p);
            continue;
        }

        char auth_value[1120];
        if (key[0] != '\0') {
            snprintf(auth_value, sizeof auth_value, "%s%s", plan.bearer ? "Bearer " : "", key);
        } else {
            auth_value[0] = '\0';
        }

        const char* hdr_name = key[0] != '\0' ? plan.auth_header : NULL;
        const char* hdr_value = key[0] != '\0' ? auth_value : NULL;
        const char* extra_name = plan.extra_header[0] != '\0' ? plan.extra_header : NULL;
        const char* extra_value = plan.extra_header[0] != '\0' ? "2023-06-01" : NULL;

        int  us = 0;
        long lat_ns = 0;
        int  prc = upstream_probe(
            plan.url, hdr_name, hdr_value, extra_name, extra_value, 5000L, &us, &lat_ns);
        long lat_ms = lat_ns > 0 ? (long)(lat_ns / 1000000L) : 0;

        health_prober_record_result(hp, p->id, p->name, p->endpoint, p->provider_type, us, lat_ms, prc);
        provider_rec_free(p);
    }

    free(recs);
    pthread_mutex_lock(&hp->lock);
    hp->last_full_probe_ts = time(NULL);
    pthread_mutex_unlock(&hp->lock);
    return 0;
}

char*
health_prober_to_json(health_prober_t* hp)
{
    if (hp == NULL) {
        return strdup("{\"providers\":[],\"total\":0}");
    }

    pthread_mutex_lock(&hp->lock);
    json_t* root = json_object();
    json_t* arr = json_array();

    int healthy_cnt = 0, degraded_cnt = 0, down_cnt = 0, paused_cnt = 0;
    for (int i = 0; i < hp->n_providers; i++) {
        provider_health_t* ph = &hp->providers[i];
        json_t*            o = json_object();
        json_object_set_new(o, "id", json_integer(ph->id));
        json_object_set_new(o, "name", json_string(ph->provider_name));
        json_object_set_new(o, "endpoint", json_string(ph->endpoint));
        json_object_set_new(o, "provider_type", json_string(ph->provider_type));
        json_object_set_new(o, "status", json_string(health_status_str(ph->status)));
        json_object_set_new(o, "latency_ms", json_integer(ph->latency_ms));
        json_object_set_new(o, "last_http_status", json_integer(ph->last_http_status));
        json_object_set_new(o, "last_check_ts", json_integer(ph->last_check_ts));
        json_object_set_new(o, "consecutive_failures", json_integer(ph->consecutive_failures));
        json_object_set_new(o, "consecutive_successes", json_integer(ph->consecutive_successes));
        json_object_set_new(o, "error", json_string(ph->last_error));

        json_array_append_new(arr, o);

        if (ph->status == HEALTH_STATUS_HEALTHY) {
            healthy_cnt++;
        } else if (ph->status == HEALTH_STATUS_DEGRADED) {
            degraded_cnt++;
        } else if (ph->status == HEALTH_STATUS_DOWN) {
            down_cnt++;
        } else if (ph->status == HEALTH_STATUS_PAUSED) {
            paused_cnt++;
        }
    }

    json_object_set_new(root, "providers", arr);
    json_object_set_new(root, "total", json_integer(hp->n_providers));
    json_object_set_new(root, "healthy", json_integer(healthy_cnt));
    json_object_set_new(root, "degraded", json_integer(degraded_cnt));
    json_object_set_new(root, "down", json_integer(down_cnt));
    json_object_set_new(root, "paused", json_integer(paused_cnt));
    json_object_set_new(root, "prober_interval_s", json_integer(hp->interval_sec));
    json_object_set_new(root, "checked_at", json_integer(hp->last_full_probe_ts));
    pthread_mutex_unlock(&hp->lock);

    char* out = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return out;
}
