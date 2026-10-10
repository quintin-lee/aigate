/** @file ip_ban_table.c
 *  @brief In-memory concurrent IP ban table with read-write locks and lazy TTL expiration.
 */
#include "policy/ip_ban_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint32_t
ip_hash(const char* ip)
{
    uint32_t hash = 5381;
    while (*ip != '\0') {
        hash = ((hash << 5) + hash) + (uint8_t)(*ip);
        ip++;
    }
    return hash % IP_BAN_BUCKETS;
}

ip_ban_table_t*
ip_ban_table_create(void)
{
    ip_ban_table_t* tbl = calloc(1, sizeof(*tbl));
    if (tbl == NULL) {
        return NULL;
    }
    if (pthread_rwlock_init(&tbl->rwlock, NULL) != 0) {
        free(tbl);
        return NULL;
    }
    tbl->count = 0;
    return tbl;
}

void
ip_ban_table_destroy(ip_ban_table_t* tbl)
{
    if (tbl == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&tbl->rwlock);
    for (size_t i = 0; i < IP_BAN_BUCKETS; i++) {
        ip_ban_entry_t* cur = tbl->buckets[i];
        while (cur != NULL) {
            ip_ban_entry_t* next = cur->next;
            free(cur);
            cur = next;
        }
        tbl->buckets[i] = NULL;
    }
    tbl->count = 0;
    pthread_rwlock_unlock(&tbl->rwlock);
    pthread_rwlock_destroy(&tbl->rwlock);
    free(tbl);
}

bool
ip_ban_table_is_banned(ip_ban_table_t* tbl, const char* ip, char* out_reason, size_t cap)
{
    if (tbl == NULL || ip == NULL || ip[0] == '\0') {
        return false;
    }

    int64_t  now = (int64_t)time(NULL);
    uint32_t b = ip_hash(ip);

    pthread_rwlock_rdlock(&tbl->rwlock);
    ip_ban_entry_t* cur = tbl->buckets[b];
    while (cur != NULL) {
        if (strcmp(cur->info.ip, ip) == 0) {
            if (cur->info.expires_at_sec <= now) {
                /* Expired ban */
                pthread_rwlock_unlock(&tbl->rwlock);
                return false;
            }
            __atomic_fetch_add(&cur->info.hit_count, 1, __ATOMIC_RELAXED);
            if (out_reason != NULL && cap > 0) {
                snprintf(out_reason, cap, "%s", cur->info.reason);
            }
            pthread_rwlock_unlock(&tbl->rwlock);
            return true;
        }
        cur = cur->next;
    }
    pthread_rwlock_unlock(&tbl->rwlock);
    return false;
}

int
ip_ban_table_ban(ip_ban_table_t* tbl, const char* ip, int64_t ttl_sec, const char* reason)
{
    if (tbl == NULL || ip == NULL || ip[0] == '\0') {
        return -1;
    }

    if (ttl_sec <= 0) {
        ttl_sec = 3600;
    }

    int64_t  now = (int64_t)time(NULL);
    int64_t  expires_at = now + ttl_sec;
    uint32_t b = ip_hash(ip);

    pthread_rwlock_wrlock(&tbl->rwlock);
    ip_ban_entry_t* cur = tbl->buckets[b];
    while (cur != NULL) {
        if (strcmp(cur->info.ip, ip) == 0) {
            /* Update existing entry */
            cur->info.expires_at_sec = expires_at;
            if (reason != NULL) {
                snprintf(cur->info.reason, sizeof(cur->info.reason), "%s", reason);
            }
            pthread_rwlock_unlock(&tbl->rwlock);
            return 0;
        }
        cur = cur->next;
    }

    /* Allocate new entry */
    ip_ban_entry_t* new_ent = calloc(1, sizeof(*new_ent));
    if (new_ent == NULL) {
        pthread_rwlock_unlock(&tbl->rwlock);
        return -1;
    }

    snprintf(new_ent->info.ip, sizeof(new_ent->info.ip), "%s", ip);
    new_ent->info.expires_at_sec = expires_at;
    if (reason != NULL) {
        snprintf(new_ent->info.reason, sizeof(new_ent->info.reason), "%s", reason);
    }
    new_ent->info.hit_count = 0;
    new_ent->next = tbl->buckets[b];
    tbl->buckets[b] = new_ent;
    tbl->count++;

    pthread_rwlock_unlock(&tbl->rwlock);
    return 0;
}

int
ip_ban_table_unban(ip_ban_table_t* tbl, const char* ip)
{
    if (tbl == NULL || ip == NULL || ip[0] == '\0') {
        return -1;
    }

    uint32_t        b = ip_hash(ip);
    ip_ban_entry_t* prev = NULL;

    pthread_rwlock_wrlock(&tbl->rwlock);
    ip_ban_entry_t* cur = tbl->buckets[b];
    while (cur != NULL) {
        if (strcmp(cur->info.ip, ip) == 0) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                tbl->buckets[b] = cur->next;
            }
            free(cur);
            if (tbl->count > 0) {
                tbl->count--;
            }
            pthread_rwlock_unlock(&tbl->rwlock);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_rwlock_unlock(&tbl->rwlock);
    return -1;
}

size_t
ip_ban_table_list(ip_ban_table_t* tbl, ip_ban_info_t* out_arr, size_t max_count)
{
    if (tbl == NULL || out_arr == NULL || max_count == 0) {
        return 0;
    }

    int64_t now = (int64_t)time(NULL);
    size_t  written = 0;

    pthread_rwlock_rdlock(&tbl->rwlock);
    for (size_t i = 0; i < IP_BAN_BUCKETS && written < max_count; i++) {
        ip_ban_entry_t* cur = tbl->buckets[i];
        while (cur != NULL && written < max_count) {
            if (cur->info.expires_at_sec > now) {
                out_arr[written++] = cur->info;
            }
            cur = cur->next;
        }
    }
    pthread_rwlock_unlock(&tbl->rwlock);
    return written;
}
