/** @file model_router.c
 *  @brief Model routing + upstream-key resolution (see model_router.h). */
#include "model_router.h"
#include "aigate_log.h"
#include "lru.h"
#include "secrets.h"

#include <stdlib.h>
#include <string.h>

static void
free_route_cb(void* val)
{
    model_rec_t* m = val;
    free(m);
}

model_router_t*
model_router_new(pg_store_t* ps, const uint8_t* master)
{
    const pg_ops_t* ops = pg_store_ops(ps);
    if (ops == NULL) {
        return NULL;
    }
    model_router_t* mr = calloc(1, sizeof *mr);
    if (mr == NULL) {
        return NULL;
    }
    mr->ops = *ops;
    mr->ops_ctx = ops->ctx;
    mr->have_master = master != NULL;
    if (master != NULL) {
        memcpy(mr->master, master, 32);
    }
    mr->routes = lru_new(1024, free_route_cb);
    if (mr->routes == NULL) {
        free(mr);
        return NULL;
    }
    return mr;
}

void
model_router_free(model_router_t* mr)
{
    if (mr == NULL) {
        return;
    }
    lru_free(mr->routes);
    free(mr);
}

#include <stdatomic.h>
#include <stdbool.h>

static _Atomic unsigned long g_rr_counter = 0;

static int
resolve_single_key(model_router_t* mr,
                   const char*     key_ref,
                   char*           out_key,
                   size_t          out_sz,
                   const char*     model_name)
{
    if (key_ref == NULL || key_ref[0] == '\0') {
        out_key[0] = '\0';
        return 0; /* no auth needed (local ollama etc.) */
    }
    if (strncmp(key_ref, "env:", 4) == 0) {
        const char* env = getenv(key_ref + 4);
        if (env == NULL || env[0] == '\0') {
            AIGATE_LOG_ERROR(
                "env key %s missing for model %s", key_ref + 4, model_name);
            return -1;
        }
        snprintf(out_key, out_sz, "%s", env);
        return 0;
    }
    if (strncmp(key_ref, "pg:", 3) == 0) {
        if (!mr->have_master) {
            AIGATE_LOG_ERROR("pg secret ref but no AIGATE_MASTER_KEY");
            return -1;
        }
        if (secret_decrypt(mr->master,
                           key_ref + 3,
                           out_key,
                           out_sz,
                           NULL) != 0) {
            AIGATE_LOG_ERROR("secret_decrypt failed for model %s", model_name);
            return -1;
        }
        return 0;
    }
    AIGATE_LOG_ERROR("unknown key ref format: %s", key_ref);
    return -1;
}

static int
resolve_key(model_router_t* mr, const model_rec_t* rec, model_rec_t* out)
{
    int rc = 0;
    /* Primary key ref */
    if (rec->upstream_key_ref[0] != '\0') {
        if (resolve_single_key(mr, rec->upstream_key_ref, out->upstream_key, sizeof out->upstream_key, rec->name) != 0) {
            rc = -1;
        }
    } else {
        out->upstream_key[0] = '\0';
    }

    /* Target key refs */
    for (int i = 0; i < out->n_targets && i < MAX_TARGETS_PER_MODEL; i++) {
        upstream_target_t* tgt = &out->targets[i];
        const char* ref = tgt->upstream_key_ref[0] != '\0' ? tgt->upstream_key_ref : rec->upstream_key_ref;
        if (ref[0] != '\0') {
            if (resolve_single_key(mr, ref, tgt->upstream_key, sizeof tgt->upstream_key, rec->name) != 0) {
                tgt->upstream_key[0] = '\0';
            }
        } else {
            tgt->upstream_key[0] = '\0';
        }
    }

    /* Harmonize primary and target 0 keys if one is set and the other is empty */
    if (out->n_targets > 0) {
        if (out->targets[0].upstream_key[0] != '\0' && out->upstream_key[0] == '\0') {
            snprintf(out->upstream_key, sizeof out->upstream_key, "%s", out->targets[0].upstream_key);
        } else if (out->upstream_key[0] != '\0' && out->targets[0].upstream_key[0] == '\0') {
            snprintf(out->targets[0].upstream_key, sizeof out->targets[0].upstream_key, "%s", out->upstream_key);
        }
    }

    return rc;
}

int
model_router_resolve(model_router_t* mr, const char* model, model_rec_t* out)
{
    model_rec_t* cached = lru_get(mr->routes, model);
    if (cached != NULL) {
        *out = *cached;
        return 0;
    }

    model_rec_t fresh;
    if (mr->ops.get_model(mr->ops_ctx, model, &fresh) != 0) {
        return -1; /* unknown or disabled model */
    }

    int rc = 0;
    if (resolve_key(mr, &fresh, &fresh) != 0) {
        rc = -1;
    }

    model_rec_t* copy = malloc(sizeof *copy);
    if (copy == NULL) {
        rc = -1;
    } else {
        *copy = fresh;
        lru_put(mr->routes, model, copy);
        *out = fresh;
    }
    return rc;
}

void
model_router_invalidate(model_router_t* mr, const char* model)
{
    if (mr->routes != NULL) {
        lru_invalidate(mr->routes, model);
    }
}

int
model_router_select_candidates(circuit_breaker_t* cb,
                               const model_rec_t* model,
                               upstream_target_t* out_candidates,
                               int                cap,
                               int*               out_count)
{
    if (model == NULL || out_candidates == NULL || cap <= 0 || out_count == NULL) {
        return -1;
    }
    *out_count = 0;

    int n_tgts = model->n_targets;
    upstream_target_t src_targets[MAX_TARGETS_PER_MODEL];
    if (n_tgts <= 0) {
        if (model->endpoint[0] == '\0') {
            return -1;
        }
        n_tgts = 1;
        memset(&src_targets[0], 0, sizeof(src_targets[0]));
        snprintf(src_targets[0].provider, sizeof(src_targets[0].provider), "%s",
                 model->provider[0] != '\0' ? model->provider : "openai");
        snprintf(src_targets[0].endpoint, sizeof(src_targets[0].endpoint), "%s", model->endpoint);
        snprintf(src_targets[0].upstream_key_ref, sizeof(src_targets[0].upstream_key_ref), "%s",
                 model->upstream_key_ref);
        snprintf(src_targets[0].upstream_key, sizeof(src_targets[0].upstream_key), "%s",
                 model->upstream_key);
        src_targets[0].weight = 1;
        src_targets[0].priority = 0;
    } else {
        if (n_tgts > MAX_TARGETS_PER_MODEL) {
            n_tgts = MAX_TARGETS_PER_MODEL;
        }
        for (int i = 0; i < n_tgts; i++) {
            src_targets[i] = model->targets[i];
            if (src_targets[i].provider[0] == '\0') {
                snprintf(src_targets[i].provider, sizeof(src_targets[i].provider), "%s",
                         model->provider[0] != '\0' ? model->provider : "openai");
            }
            if (src_targets[i].weight <= 0) {
                src_targets[i].weight = 1;
            }
            if (src_targets[i].upstream_key[0] == '\0' && model->upstream_key[0] != '\0') {
                snprintf(src_targets[i].upstream_key, sizeof(src_targets[i].upstream_key), "%s",
                         model->upstream_key);
            }
        }
    }

    /* Collect distinct priorities sorted ascending */
    int prios[MAX_TARGETS_PER_MODEL];
    int n_prios = 0;
    for (int i = 0; i < n_tgts; i++) {
        int p = src_targets[i].priority;
        bool found = false;
        for (int j = 0; j < n_prios; j++) {
            if (prios[j] == p) {
                found = true;
                break;
            }
        }
        if (!found) {
            prios[n_prios++] = p;
        }
    }
    /* Insertion sort priorities */
    for (int i = 0; i < n_prios - 1; i++) {
        for (int j = i + 1; j < n_prios; j++) {
            if (prios[j] < prios[i]) {
                int tmp = prios[i];
                prios[i] = prios[j];
                prios[j] = tmp;
            }
        }
    }

    int total_added = 0;
    int healthy_count = 0;

    /* First pass: count healthy targets */
    for (int i = 0; i < n_tgts; i++) {
        if (cb == NULL || cb_allow_request(cb, model->name, src_targets[i].endpoint)) {
            healthy_count++;
        }
    }

    if (healthy_count > 0) {
        /* Iterate through priority tiers ascending */
        for (int pi = 0; pi < n_prios && total_added < cap; pi++) {
            int p = prios[pi];
            int tier_healthy_idx[MAX_TARGETS_PER_MODEL];
            int n_th = 0;
            for (int i = 0; i < n_tgts; i++) {
                if (src_targets[i].priority == p) {
                    if (cb == NULL || cb_allow_request(cb, model->name, src_targets[i].endpoint)) {
                        tier_healthy_idx[n_th++] = i;
                    }
                }
            }
            if (n_th == 0) {
                continue; /* Skip empty / fully tripped tier */
            }

            /* Apply LB policy within this tier */
            if (strcmp(model->lb_policy, "round_robin") == 0 && n_th > 1) {
                unsigned long start = atomic_fetch_add_explicit(&g_rr_counter, 1, memory_order_relaxed) % (unsigned long)n_th;
                for (int k = 0; k < n_th && total_added < cap; k++) {
                    int src_idx = tier_healthy_idx[(start + (unsigned long)k) % (unsigned long)n_th];
                    out_candidates[total_added++] = src_targets[src_idx];
                }
            } else if ((strcmp(model->lb_policy, "weighted") == 0 || strcmp(model->lb_policy, "weighted_round_robin") == 0) && n_th > 1) {
                int total_w = 0;
                for (int k = 0; k < n_th; k++) {
                    total_w += src_targets[tier_healthy_idx[k]].weight;
                }
                if (total_w <= 0) total_w = n_th;
                unsigned long pick = atomic_fetch_add_explicit(&g_rr_counter, 1, memory_order_relaxed) % (unsigned long)total_w;
                int chosen_k = 0;
                int acc = 0;
                for (int k = 0; k < n_th; k++) {
                    acc += src_targets[tier_healthy_idx[k]].weight;
                    if ((unsigned long)acc > pick) {
                        chosen_k = k;
                        break;
                    }
                }
                /* Add chosen target first */
                out_candidates[total_added++] = src_targets[tier_healthy_idx[chosen_k]];
                /* Add remaining targets sorted by weight descending */
                int rem_k[MAX_TARGETS_PER_MODEL];
                int n_rem = 0;
                for (int k = 0; k < n_th; k++) {
                    if (k != chosen_k) {
                        rem_k[n_rem++] = tier_healthy_idx[k];
                    }
                }
                for (int a = 0; a < n_rem - 1; a++) {
                    for (int b = a + 1; b < n_rem; b++) {
                        if (src_targets[rem_k[b]].weight > src_targets[rem_k[a]].weight) {
                            int tmp = rem_k[a];
                            rem_k[a] = rem_k[b];
                            rem_k[b] = tmp;
                        }
                    }
                }
                for (int k = 0; k < n_rem && total_added < cap; k++) {
                    out_candidates[total_added++] = src_targets[rem_k[k]];
                }
            } else {
                /* Default "priority": retain original definition order */
                for (int k = 0; k < n_th && total_added < cap; k++) {
                    out_candidates[total_added++] = src_targets[tier_healthy_idx[k]];
                }
            }
        }
    } else {
        /* ALL targets are tripped: fallback to target with earliest open_until */
        struct tripped_tgt {
            int    src_idx;
            time_t open_until;
        } tripped[MAX_TARGETS_PER_MODEL];

        for (int i = 0; i < n_tgts; i++) {
            tripped[i].src_idx = i;
            tripped[i].open_until = cb_get_open_until(cb, model->name, src_targets[i].endpoint);
        }
        /* Sort tripped targets by open_until ascending, then by priority ascending */
        for (int i = 0; i < n_tgts - 1; i++) {
            for (int j = i + 1; j < n_tgts; j++) {
                if (tripped[j].open_until < tripped[i].open_until ||
                    (tripped[j].open_until == tripped[i].open_until &&
                     src_targets[tripped[j].src_idx].priority < src_targets[tripped[i].src_idx].priority)) {
                    struct tripped_tgt tmp = tripped[i];
                    tripped[i] = tripped[j];
                    tripped[j] = tmp;
                }
            }
        }
        for (int i = 0; i < n_tgts && total_added < cap; i++) {
            out_candidates[total_added++] = src_targets[tripped[i].src_idx];
        }
    }

    *out_count = total_added;
    return 0;
}
