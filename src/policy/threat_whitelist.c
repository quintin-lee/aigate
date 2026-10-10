/**
 * @file threat_whitelist.c
 * @brief Dynamic threat defense whitelist engine implementation.
 */
#include "threat_whitelist.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THREAT_WHITELIST_INITIAL_CAP 64

struct threat_whitelist {
    pthread_rwlock_t        rwlock;
    threat_whitelist_rec_t* rules;
    size_t                  count;
    size_t                  capacity;
};

threat_whitelist_t*
threat_whitelist_create(void)
{
    threat_whitelist_t* tw = calloc(1, sizeof(*tw));
    if (tw == NULL) {
        return NULL;
    }
    if (pthread_rwlock_init(&tw->rwlock, NULL) != 0) {
        free(tw);
        return NULL;
    }
    tw->capacity = THREAT_WHITELIST_INITIAL_CAP;
    tw->rules = calloc(tw->capacity, sizeof(threat_whitelist_rec_t));
    if (tw->rules == NULL) {
        pthread_rwlock_destroy(&tw->rwlock);
        free(tw);
        return NULL;
    }
    tw->count = 0;
    return tw;
}

void
threat_whitelist_destroy(threat_whitelist_t* tw)
{
    if (tw == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&tw->rwlock);
    if (tw->rules != NULL) {
        free(tw->rules);
        tw->rules = NULL;
    }
    tw->count = 0;
    tw->capacity = 0;
    pthread_rwlock_unlock(&tw->rwlock);
    pthread_rwlock_destroy(&tw->rwlock);
    free(tw);
}

int
threat_whitelist_add(threat_whitelist_t* tw, const threat_whitelist_rec_t* rec)
{
    if (tw == NULL || rec == NULL) {
        return -1;
    }
    pthread_rwlock_wrlock(&tw->rwlock);

    /* 1. Update existing rule if rule_id already present */
    for (size_t i = 0; i < tw->count; i++) {
        if (tw->rules[i].rule_id == rec->rule_id) {
            tw->rules[i] = *rec;
            pthread_rwlock_unlock(&tw->rwlock);
            return 0;
        }
    }

    /* 2. Expand capacity if needed */
    if (tw->count >= tw->capacity) {
        size_t new_cap = tw->capacity * 2;
        if (new_cap < THREAT_WHITELIST_INITIAL_CAP) {
            new_cap = THREAT_WHITELIST_INITIAL_CAP;
        }
        threat_whitelist_rec_t* new_rules =
            realloc(tw->rules, new_cap * sizeof(threat_whitelist_rec_t));
        if (new_rules == NULL) {
            pthread_rwlock_unlock(&tw->rwlock);
            return -1;
        }
        tw->rules = new_rules;
        tw->capacity = new_cap;
    }

    tw->rules[tw->count++] = *rec;
    pthread_rwlock_unlock(&tw->rwlock);
    return 0;
}

int
threat_whitelist_remove(threat_whitelist_t* tw, int64_t rule_id)
{
    if (tw == NULL) {
        return -1;
    }
    pthread_rwlock_wrlock(&tw->rwlock);
    for (size_t i = 0; i < tw->count; i++) {
        if (tw->rules[i].rule_id == rule_id) {
            /* Shift elements to preserve order */
            for (size_t j = i; j + 1 < tw->count; j++) {
                tw->rules[j] = tw->rules[j + 1];
            }
            tw->count--;
            pthread_rwlock_unlock(&tw->rwlock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&tw->rwlock);
    return -1;
}

int
threat_whitelist_load(threat_whitelist_t* tw, const threat_whitelist_rec_t* rules, size_t count)
{
    if (tw == NULL) {
        return -1;
    }
    pthread_rwlock_wrlock(&tw->rwlock);

    size_t new_cap = count > THREAT_WHITELIST_INITIAL_CAP ? count : THREAT_WHITELIST_INITIAL_CAP;
    threat_whitelist_rec_t* new_rules = calloc(new_cap, sizeof(threat_whitelist_rec_t));
    if (new_rules == NULL) {
        pthread_rwlock_unlock(&tw->rwlock);
        return -1;
    }
    if (rules != NULL && count > 0) {
        memcpy(new_rules, rules, count * sizeof(threat_whitelist_rec_t));
    }

    free(tw->rules);
    tw->rules = new_rules;
    tw->count = count;
    tw->capacity = new_cap;

    pthread_rwlock_unlock(&tw->rwlock);
    return 0;
}

bool
threat_whitelist_is_bypassed(threat_whitelist_t* tw,
                             uint64_t            key_id,
                             const char*         model,
                             const char*         rule_tag,
                             int64_t*            matched_rule_id)
{
    if (tw == NULL) {
        return false;
    }
    time_t now = time(NULL);

    pthread_rwlock_rdlock(&tw->rwlock);
    for (size_t i = 0; i < tw->count; i++) {
        const threat_whitelist_rec_t* r = &tw->rules[i];
        if (!r->enabled) {
            continue;
        }
        if (r->expires_at > 0 && r->expires_at <= (int64_t)now) {
            continue;
        }
        if (r->match_key_id != 0 && r->match_key_id != key_id) {
            continue;
        }
        if (r->match_model[0] != '\0' && strcmp(r->match_model, "*") != 0) {
            if (model == NULL || strcmp(r->match_model, model) != 0) {
                continue;
            }
        }
        if (r->bypass_rule_tag[0] != '\0' && strcmp(r->bypass_rule_tag, "*") != 0) {
            if (rule_tag == NULL || strcmp(r->bypass_rule_tag, rule_tag) != 0) {
                continue;
            }
        }

        /* Match found */
        if (matched_rule_id != NULL) {
            *matched_rule_id = r->rule_id;
        }
        pthread_rwlock_unlock(&tw->rwlock);
        return true;
    }
    pthread_rwlock_unlock(&tw->rwlock);
    return false;
}

int
threat_whitelist_list(threat_whitelist_t*     tw,
                      threat_whitelist_rec_t* out,
                      int                     cap,
                      int*                    out_n)
{
    if (tw == NULL || out == NULL || out_n == NULL || cap <= 0) {
        return -1;
    }
    pthread_rwlock_rdlock(&tw->rwlock);
    int to_copy = (int)tw->count < cap ? (int)tw->count : cap;
    if (to_copy > 0) {
        memcpy(out, tw->rules, (size_t)to_copy * sizeof(threat_whitelist_rec_t));
    }
    *out_n = to_copy;
    pthread_rwlock_unlock(&tw->rwlock);
    return 0;
}
