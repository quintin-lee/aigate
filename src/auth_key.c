/** @file auth_key.c
 *  @brief Client key verification with LRU caching (see auth_key.h). */
#include "auth_key.h"
#include "aigate_log.h"
#include "lru.h"
#include "sha256.h"

#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void free_rec_cb(void *val)
{
  key_rec_t *k = val;
  key_rec_free(k);
  free(k);
}

int auth_key_init(auth_key_cache *akc, pg_store_t *ps)
{
  const pg_ops_t *ops = pg_store_ops(ps);
  if (ops == NULL)
    return -1;
  akc->ops = *ops;
  akc->ops_ctx = ops->ctx;
  akc->recs = lru_new(4096, free_rec_cb);
  return akc->recs != NULL ? 0 : -1;
}

void auth_key_shutdown(auth_key_cache *akc)
{
  if (akc == NULL)
    return;
  lru_free(akc->recs);
  akc->recs = NULL;
}

/** @brief Deep-copy a record; @p dst's allowlist becomes heap-owned.
 * @return 0 ok, -1 allocation failure (dst zeroed). */
static int deep_copy_rec(key_rec_t *dst, const key_rec_t *src)
{
  key_rec_free(dst);
  *dst = *src;
  dst->allowed_models = NULL;
  dst->n_allowed = src->n_allowed;
  if (dst->n_allowed > 0) {
    dst->allowed_models = malloc(sizeof(char *) * (size_t)dst->n_allowed);
    if (dst->allowed_models == NULL)
      return -1;
    for (int i = 0; i < dst->n_allowed; i++) {
      dst->allowed_models[i] = strdup(src->allowed_models[i]);
      if (dst->allowed_models[i] == NULL)
        return -1;
    }
  }
  return 0;
}

int auth_key_resolve(auth_key_cache *akc, const char *bearer, key_rec_t *out)
{
  char hash[65];
  key_rec_t *cached;

  memset(out, 0, sizeof *out);
  if (bearer == NULL || bearer[0] == '\0')
    return -1;
  if (sha256_hex(bearer, strlen(bearer), hash) != 0)
    return -1;

  cached = lru_get(akc->recs, hash);
  if (cached != NULL) {
    /* caller receives an owned copy; cache keeps its own */
    if (deep_copy_rec(out, cached) != 0)
      return -1;
    if (cached->revoked)
      return -2;
    if (cached->has_expiry && cached->expires_at < time(NULL))
      return -3;
    return 0;
  }

  key_rec_t fresh;
  memset(&fresh, 0, sizeof fresh);
  if (akc->ops.get_key_by_hash(akc->ops_ctx, hash, &fresh) != 0)
    return -1;

  key_rec_t *copy = malloc(sizeof *copy);
  if (copy == NULL) {
    key_rec_free(&fresh);
    return -1;
  }

  /* Resolve the outcome from the stack record while it still fully owns
   * its fields; negative results are not cached. */
  int result = 0;
  if (fresh.revoked)
    result = -2;
  else if (fresh.has_expiry && fresh.expires_at < time(NULL))
    result = -3;

  if (result == 0) {
    if (deep_copy_rec(out, &fresh) != 0) {
      key_rec_free(&fresh);
      free(copy);
      return -1;
    }
    *copy = fresh;
    fresh.allowed_models = NULL; /* the array is now owned by copy */
    fresh.n_allowed = 0;
    /* The LRU owns copy; a same-key replacement frees the old value. */
    lru_put(akc->recs, hash, copy);
    return 0;
  }

  /* revoked / expired: release both records, do not cache the negative. */
  key_rec_free(&fresh);
  free(copy);
  return result;
}

void auth_key_invalidate(auth_key_cache *akc, const char *key_hash)
{
  /* lru_invalidate invokes the evict callback (free_rec_cb) for the
   * removed record, which frees the allowlist and the record itself. */
  lru_invalidate(akc->recs, key_hash);
}

int key_allows_model(const key_rec_t *k, const char *model)
{
  if (k == NULL)
    return 0;
  if (model == NULL)
    return 1;
  if (k->n_allowed == 0)
    return 1; /* empty allowlist = all */
  for (int i = 0; i < k->n_allowed; i++)
    if (strcmp(k->allowed_models[i], model) == 0)
      return 1;
  return 0;
}
