/** @file response_cache.c
 *  @brief Implementation of sharded LRU response cache with canonical fingerprinting.
 */
#include "response_cache.h"
#include "aigate_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <openssl/sha.h>
#include <jansson.h>

static inline int
shard_index(const char* key)
{
    if (key == NULL || key[0] == '\0' || key[1] == '\0') {
        return 0;
    }
    unsigned int v = 0;
    char c0 = key[0];
    char c1 = key[1];
    if (c0 >= '0' && c0 <= '9') v = (c0 - '0') << 4;
    else if (c0 >= 'a' && c0 <= 'f') v = (c0 - 'a' + 10) << 4;
    else if (c0 >= 'A' && c0 <= 'F') v = (c0 - 'A' + 10) << 4;

    if (c1 >= '0' && c1 <= '9') v |= (c1 - '0');
    else if (c1 >= 'a' && c1 <= 'f') v |= (c1 - 'a' + 10);
    else if (c1 >= 'A' && c1 <= 'F') v |= (c1 - 'A' + 10);

    return (int)(v % CACHE_SHARDS_COUNT);
}

static inline unsigned int
bucket_index(const char* key)
{
    unsigned int hash = 5381;
    const unsigned char* p = (const unsigned char*)key;
    while (*p) {
        hash = ((hash << 5) + hash) + *p++;
    }
    return hash % CACHE_BUCKETS_PER_SHARD;
}

static void
lru_remove(cache_shard_t* shard, cache_entry_t* entry)
{
    if (entry->prev != NULL) {
        entry->prev->next = entry->next;
    } else {
        shard->lru_head = entry->next;
    }
    if (entry->next != NULL) {
        entry->next->prev = entry->prev;
    } else {
        shard->lru_tail = entry->prev;
    }
    entry->prev = NULL;
    entry->next = NULL;
}

static void
lru_push_head(cache_shard_t* shard, cache_entry_t* entry)
{
    entry->prev = NULL;
    entry->next = shard->lru_head;
    if (shard->lru_head != NULL) {
        shard->lru_head->prev = entry;
    }
    shard->lru_head = entry;
    if (shard->lru_tail == NULL) {
        shard->lru_tail = entry;
    }
}

static void
destroy_entry_internal(cache_entry_t* entry)
{
    if (entry == NULL) {
        return;
    }
    if (atomic_load(&entry->ref_count) <= 0) {
        if (entry->response_body != NULL) {
            free(entry->response_body);
            entry->response_body = NULL;
        }
        free(entry);
    } else {
        /* Entry is still referenced by a reader: unlink its data marker */
        entry->status_code = -1;
    }
}

static void
evict_one_lru_locked(cache_shard_t* shard)
{
    cache_entry_t* victim = shard->lru_tail;
    if (victim == NULL) {
        return;
    }

    /* Unlink from hash bucket */
    unsigned int b = bucket_index(victim->cache_key);
    cache_entry_t** curr = &shard->buckets[b];
    while (*curr != NULL) {
        if (*curr == victim) {
            *curr = victim->hnext;
            break;
        }
        curr = &(*curr)->hnext;
    }

    /* Unlink from LRU list */
    lru_remove(shard, victim);

    if (shard->count > 0) {
        shard->count--;
    }
    if (shard->bytes_used >= victim->response_len) {
        shard->bytes_used -= victim->response_len;
    } else {
        shard->bytes_used = 0;
    }

    destroy_entry_internal(victim);
}

response_cache_t*
response_cache_new(size_t max_bytes, size_t max_entries, long default_ttl_sec)
{
    response_cache_t* rc = calloc(1, sizeof(*rc));
    if (rc == NULL) {
        return NULL;
    }

    rc->enabled = 1;
    rc->total_max_bytes = max_bytes > 0 ? max_bytes : (128UL * 1024UL * 1024UL);
    rc->total_max_entries = max_entries > 0 ? max_entries : 20000;
    rc->default_ttl_sec = default_ttl_sec > 0 ? default_ttl_sec : 3600;

    size_t shard_max_bytes = rc->total_max_bytes / CACHE_SHARDS_COUNT;
    size_t shard_max_entries = rc->total_max_entries / CACHE_SHARDS_COUNT;
    if (shard_max_bytes == 0) shard_max_bytes = 1024 * 1024;
    if (shard_max_entries == 0) shard_max_entries = 100;

    for (int i = 0; i < CACHE_SHARDS_COUNT; i++) {
        pthread_mutex_init(&rc->shards[i].lock, NULL);
        rc->shards[i].max_bytes = shard_max_bytes;
        rc->shards[i].max_count = shard_max_entries;
    }

    atomic_init(&rc->total_saved_prompt_tokens, 0);
    atomic_init(&rc->total_saved_completion_tokens, 0);
    atomic_init(&rc->total_saved_cost_usd, 0.0);

    return rc;
}

void
response_cache_free(response_cache_t* rc)
{
    if (rc == NULL) {
        return;
    }

    for (int i = 0; i < CACHE_SHARDS_COUNT; i++) {
        cache_shard_t* shard = &rc->shards[i];
        pthread_mutex_lock(&shard->lock);
        cache_entry_t* cur = shard->lru_head;
        while (cur != NULL) {
            cache_entry_t* next = cur->next;
            if (cur->response_body != NULL) {
                free(cur->response_body);
            }
            free(cur);
            cur = next;
        }
        shard->lru_head = NULL;
        shard->lru_tail = NULL;
        memset(shard->buckets, 0, sizeof(shard->buckets));
        shard->count = 0;
        shard->bytes_used = 0;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_destroy(&shard->lock);
    }

    free(rc);
}

int
response_cache_fingerprint(const char* model, const char* json_body, size_t body_len, char out_key[65])
{
    if (model == NULL || out_key == NULL) {
        return -1;
    }
    if (json_body == NULL || body_len == 0) {
        /* Fallback simple hash on model alone */
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char*)model, strlen(model), hash);
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            sprintf(out_key + i * 2, "%02x", hash[i]);
        }
        out_key[64] = '\0';
        return 0;
    }

    json_error_t err;
    json_t* root = json_loadb(json_body, body_len, 0, &err);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return -1;
    }

    /* Build canonical normalized string */
    char buf[16384];
    size_t off = 0;

    /* 1. Model */
    char model_lower[64];
    size_t ml = strlen(model);
    if (ml >= sizeof(model_lower)) ml = sizeof(model_lower) - 1;
    for (size_t i = 0; i < ml; i++) {
        model_lower[i] = (char)tolower((unsigned char)model[i]);
    }
    model_lower[ml] = '\0';
    off += snprintf(buf + off, sizeof(buf) - off, "model:%s|", model_lower);

    /* 2. Temperature & Top_p */
    json_t* j_temp = json_object_get(root, "temperature");
    if (json_is_number(j_temp) && off < sizeof(buf) - 32) {
        off += snprintf(buf + off, sizeof(buf) - off, "temp:%.2f|", json_number_value(j_temp));
    }
    json_t* j_topp = json_object_get(root, "top_p");
    if (json_is_number(j_topp) && off < sizeof(buf) - 32) {
        off += snprintf(buf + off, sizeof(buf) - off, "topp:%.2f|", json_number_value(j_topp));
    }

    /* 3. System prompt */
    json_t* j_sys = json_object_get(root, "system");
    if (json_is_string(j_sys) && off < sizeof(buf) - 64) {
        off += snprintf(buf + off, sizeof(buf) - off, "sys:%s|", json_string_value(j_sys));
    }

    /* 4. Messages / contents */
    json_t* j_msgs = json_object_get(root, "messages");
    if (!json_is_array(j_msgs)) {
        j_msgs = json_object_get(root, "contents"); /* Gemini format */
    }
    if (json_is_array(j_msgs)) {
        size_t idx;
        json_t* m;
        json_array_foreach(j_msgs, idx, m) {
            if (!json_is_object(m)) continue;
            const char* role = json_string_value(json_object_get(m, "role"));
            json_t* content = json_object_get(m, "content");
            if (content == NULL) {
                content = json_object_get(m, "parts");
            }
            if (role != NULL && off < sizeof(buf) - 128) {
                off += snprintf(buf + off, sizeof(buf) - off, "msg[%zu]:%s=", idx, role);
            }
            if (json_is_string(content) && off < sizeof(buf) - 128) {
                const char* cstr = json_string_value(content);
                size_t clen = strlen(cstr);
                if (clen > 256) clen = 256; /* bounded representation */
                if (off + clen < sizeof(buf) - 10) {
                    memcpy(buf + off, cstr, clen);
                    off += clen;
                    buf[off++] = '|';
                }
            } else if (json_is_array(content)) {
                size_t pidx;
                json_t* part;
                json_array_foreach(content, pidx, part) {
                    if (json_is_object(part)) {
                        const char* pt = json_string_value(json_object_get(part, "text"));
                        if (pt != NULL && off < sizeof(buf) - 128) {
                            off += snprintf(buf + off, sizeof(buf) - off, "part[%zu]:%s|", pidx, pt);
                        }
                    }
                }
            }
            if (off >= sizeof(buf) - 256) {
                break;
            }
        }
    }

    /* 5. Tools / Functions */
    json_t* j_tools = json_object_get(root, "tools");
    if (json_is_array(j_tools) && off < sizeof(buf) - 128) {
        off += snprintf(buf + off, sizeof(buf) - off, "tools:");
        size_t tidx;
        json_t* tool;
        json_array_foreach(j_tools, tidx, tool) {
            json_t* fn = json_object_get(tool, "function");
            const char* fname = json_string_value(json_object_get(fn ? fn : tool, "name"));
            if (fname != NULL && off < sizeof(buf) - 64) {
                off += snprintf(buf + off, sizeof(buf) - off, "%s,", fname);
            }
        }
        buf[off++] = '|';
    }

    json_decref(root);
    buf[off] = '\0';

    /* SHA-256 computation */
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)buf, off, hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(out_key + i * 2, "%02x", hash[i]);
    }
    out_key[64] = '\0';
    return 0;
}

cache_entry_t*
response_cache_get(response_cache_t* rc, const char* key)
{
    if (rc == NULL || !rc->enabled || key == NULL || key[0] == '\0') {
        return NULL;
    }

    int s = shard_index(key);
    cache_shard_t* shard = &rc->shards[s];
    unsigned int b = bucket_index(key);
    time_t now = time(NULL);

    pthread_mutex_lock(&shard->lock);
    cache_entry_t* entry = shard->buckets[b];
    while (entry != NULL) {
        if (strcmp(entry->cache_key, key) == 0) {
            /* Check if expired */
            if (now >= entry->expires_at) {
                /* Expired: unlink and evict */
                cache_entry_t** curr = &shard->buckets[b];
                while (*curr != NULL) {
                    if (*curr == entry) {
                        *curr = entry->hnext;
                        break;
                    }
                    curr = &(*curr)->hnext;
                }
                lru_remove(shard, entry);
                if (shard->count > 0) shard->count--;
                if (shard->bytes_used >= entry->response_len) {
                    shard->bytes_used -= entry->response_len;
                } else {
                    shard->bytes_used = 0;
                }
                shard->misses++;
                destroy_entry_internal(entry);
                pthread_mutex_unlock(&shard->lock);
                return NULL;
            }

            /* Valid hit: update LRU position */
            lru_remove(shard, entry);
            lru_push_head(shard, entry);

            shard->hits++;
            atomic_fetch_add(&rc->total_saved_prompt_tokens, (uint64_t)entry->prompt_tokens);
            atomic_fetch_add(&rc->total_saved_completion_tokens, (uint64_t)entry->completion_tokens);
            double prev_cost = atomic_load(&rc->total_saved_cost_usd);
            while (!atomic_compare_exchange_weak(&rc->total_saved_cost_usd, &prev_cost, prev_cost + entry->cost_usd)) {}

            atomic_fetch_add(&entry->ref_count, 1);
            pthread_mutex_unlock(&shard->lock);
            return entry;
        }
        entry = entry->hnext;
    }

    shard->misses++;
    pthread_mutex_unlock(&shard->lock);
    return NULL;
}

void
response_cache_release_entry(cache_entry_t* entry)
{
    if (entry == NULL) {
        return;
    }
    int prev = atomic_fetch_sub(&entry->ref_count, 1);
    if (prev <= 1 && entry->status_code == -1) {
        /* Entry was previously evicted/unlinked from shard while referenced */
        if (entry->response_body != NULL) {
            free(entry->response_body);
            entry->response_body = NULL;
        }
        free(entry);
    }
}

int
response_cache_set(response_cache_t* rc,
                   const char*       key,
                   const char*       model,
                   const char*       body,
                   size_t            len,
                   long              prompt_tokens,
                   long              completion_tokens,
                   double            cost_usd,
                   long              ttl_sec)
{
    if (rc == NULL || !rc->enabled || key == NULL || body == NULL || len == 0) {
        return -1;
    }
    if (len > 1024 * 1024) {
        /* Skip responses larger than 1MB */
        return -1;
    }

    int s = shard_index(key);
    cache_shard_t* shard = &rc->shards[s];
    unsigned int b = bucket_index(key);
    time_t now = time(NULL);
    long ttl = ttl_sec > 0 ? ttl_sec : rc->default_ttl_sec;

    pthread_mutex_lock(&shard->lock);

    /* Check if already exists in shard */
    cache_entry_t* entry = shard->buckets[b];
    while (entry != NULL) {
        if (strcmp(entry->cache_key, key) == 0) {
            /* Update existing entry */
            char* new_body = malloc(len + 1);
            if (new_body == NULL) {
                pthread_mutex_unlock(&shard->lock);
                return -1;
            }
            memcpy(new_body, body, len);
            new_body[len] = '\0';

            if (shard->bytes_used >= entry->response_len) {
                shard->bytes_used -= entry->response_len;
            }
            free(entry->response_body);
            entry->response_body = new_body;
            entry->response_len = len;
            shard->bytes_used += len;

            entry->status_code = 200;
            entry->prompt_tokens = prompt_tokens;
            entry->completion_tokens = completion_tokens;
            entry->cost_usd = cost_usd;
            entry->created_at = now;
            entry->expires_at = now + ttl;

            lru_remove(shard, entry);
            lru_push_head(shard, entry);

            pthread_mutex_unlock(&shard->lock);
            return 0;
        }
        entry = entry->hnext;
    }

    /* Enforce shard capacity before inserting new entry */
    while (shard->count >= shard->max_count ||
           (shard->bytes_used + len > shard->max_bytes && shard->count > 0)) {
        evict_one_lru_locked(shard);
    }

    /* Allocate and initialize new entry */
    cache_entry_t* e = calloc(1, sizeof(*e));
    if (e == NULL) {
        pthread_mutex_unlock(&shard->lock);
        return -1;
    }

    e->response_body = malloc(len + 1);
    if (e->response_body == NULL) {
        free(e);
        pthread_mutex_unlock(&shard->lock);
        return -1;
    }
    memcpy(e->response_body, body, len);
    e->response_body[len] = '\0';
    e->response_len = len;

    snprintf(e->cache_key, sizeof(e->cache_key), "%s", key);
    if (model != NULL) {
        snprintf(e->model, sizeof(e->model), "%s", model);
    }
    e->status_code = 200;
    e->prompt_tokens = prompt_tokens;
    e->completion_tokens = completion_tokens;
    e->cost_usd = cost_usd;
    e->created_at = now;
    e->expires_at = now + ttl;
    atomic_init(&e->ref_count, 0);

    /* Insert into hash bucket */
    e->hnext = shard->buckets[b];
    shard->buckets[b] = e;

    /* Insert into LRU head */
    lru_push_head(shard, e);

    shard->count++;
    shard->bytes_used += len;

    pthread_mutex_unlock(&shard->lock);
    return 0;
}

int
response_cache_purge(response_cache_t* rc,
                     const char*       model_or_null,
                     size_t*           out_purged_entries,
                     size_t*           out_freed_bytes)
{
    if (rc == NULL) {
        return -1;
    }

    size_t total_purged = 0;
    size_t total_freed = 0;

    for (int i = 0; i < CACHE_SHARDS_COUNT; i++) {
        cache_shard_t* shard = &rc->shards[i];
        pthread_mutex_lock(&shard->lock);

        for (int b = 0; b < CACHE_BUCKETS_PER_SHARD; b++) {
            cache_entry_t** curr = &shard->buckets[b];
            while (*curr != NULL) {
                cache_entry_t* e = *curr;
                if (model_or_null == NULL || model_or_null[0] == '\0' ||
                    strcmp(e->model, model_or_null) == 0) {
                    *curr = e->hnext;
                    lru_remove(shard, e);

                    if (shard->count > 0) shard->count--;
                    if (shard->bytes_used >= e->response_len) {
                        shard->bytes_used -= e->response_len;
                    }
                    total_purged++;
                    total_freed += e->response_len;

                    destroy_entry_internal(e);
                } else {
                    curr = &(*curr)->hnext;
                }
            }
        }

        pthread_mutex_unlock(&shard->lock);
    }

    if (out_purged_entries != NULL) *out_purged_entries = total_purged;
    if (out_freed_bytes != NULL) *out_freed_bytes = total_freed;
    return 0;
}

char*
response_cache_get_stats_json(response_cache_t* rc)
{
    if (rc == NULL) {
        return strdup("{\"enabled\":false}");
    }

    size_t total_count = 0;
    size_t total_bytes = 0;
    uint64_t total_hits = 0;
    uint64_t total_misses = 0;

    json_t* shards_arr = json_array();
    for (int i = 0; i < CACHE_SHARDS_COUNT; i++) {
        cache_shard_t* shard = &rc->shards[i];
        pthread_mutex_lock(&shard->lock);
        total_count += shard->count;
        total_bytes += shard->bytes_used;
        total_hits += shard->hits;
        total_misses += shard->misses;

        json_t* sj = json_object();
        json_object_set_new(sj, "shard_id", json_integer(i));
        json_object_set_new(sj, "entries", json_integer((json_int_t)shard->count));
        json_object_set_new(sj, "bytes_used", json_integer((json_int_t)shard->bytes_used));
        json_object_set_new(sj, "hits", json_integer((json_int_t)shard->hits));
        json_object_set_new(sj, "misses", json_integer((json_int_t)shard->misses));
        json_array_append_new(shards_arr, sj);
        pthread_mutex_unlock(&shard->lock);
    }

    double hit_rate = 0.0;
    uint64_t total_reqs = total_hits + total_misses;
    if (total_reqs > 0) {
        hit_rate = ((double)total_hits / (double)total_reqs) * 100.0;
    }

    uint64_t saved_prompt = atomic_load(&rc->total_saved_prompt_tokens);
    uint64_t saved_comp = atomic_load(&rc->total_saved_completion_tokens);
    double saved_cost = atomic_load(&rc->total_saved_cost_usd);

    json_t* root = json_object();
    json_object_set_new(root, "enabled", json_boolean(rc->enabled));
    json_object_set_new(root, "backend", json_string("sharded_lru"));
    json_object_set_new(root, "shards", json_integer(CACHE_SHARDS_COUNT));
    json_object_set_new(root, "entries_count", json_integer(total_count));
    json_object_set_new(root, "max_entries", json_integer(rc->total_max_entries));
    json_object_set_new(root, "bytes_used", json_integer(total_bytes));
    json_object_set_new(root, "bytes_max", json_integer(rc->total_max_bytes));
    json_object_set_new(root, "hits_total", json_integer(total_hits));
    json_object_set_new(root, "misses_total", json_integer(total_misses));
    json_object_set_new(root, "hit_rate_percent", json_real(hit_rate));
    json_object_set_new(root, "saved_prompt_tokens", json_integer(saved_prompt));
    json_object_set_new(root, "saved_completion_tokens", json_integer(saved_comp));
    json_object_set_new(root, "saved_cost_usd", json_real(saved_cost));
    json_object_set_new(root, "shards_detail", shards_arr);

    char* out = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return out;
}
