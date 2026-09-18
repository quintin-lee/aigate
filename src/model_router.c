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

static int
resolve_key(model_router_t* mr, const model_rec_t* rec, model_rec_t* out)
{
    /* out->upstream_key_ref already copied; produce out->upstream_key. */
    if (rec->upstream_key_ref[0] == '\0') {
        out->upstream_key[0] = '\0';
        return 0; /* no auth needed (local ollama etc.) */
    }
    if (strncmp(rec->upstream_key_ref, "env:", 4) == 0) {
        const char* env = getenv(rec->upstream_key_ref + 4);
        if (env == NULL || env[0] == '\0') {
            AIGATE_LOG_ERROR(
                "env key %s missing for model %s", rec->upstream_key_ref + 4, rec->name);
            return -1;
        }
        snprintf(out->upstream_key, sizeof out->upstream_key, "%s", env);
        return 0;
    }
    if (strncmp(rec->upstream_key_ref, "pg:", 3) == 0) {
        if (!mr->have_master) {
            AIGATE_LOG_ERROR("pg secret ref but no AIGATE_MASTER_KEY");
            return -1;
        }
        if (secret_decrypt(mr->master,
                           rec->upstream_key_ref + 3,
                           out->upstream_key,
                           sizeof out->upstream_key,
                           NULL) != 0) {
            AIGATE_LOG_ERROR("secret_decrypt failed for model %s", rec->name);
            return -1;
        }
        return 0;
    }
    AIGATE_LOG_ERROR("unknown key ref format: %s", rec->upstream_key_ref);
    return -1;
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
