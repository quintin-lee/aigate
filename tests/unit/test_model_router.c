/** @file test_model_router.c
 *  @brief model_router resolve/invalidate + key-ref resolution tests. */
#include "run_tests.h"
#include "model_router.h"
#include <stdlib.h>
#include <string.h>

struct mro_db {
  int get_model_calls;
  model_rec_t models[4];
  int n_models;
};

static int mro_get_model(void *ctx, const char *name, model_rec_t *out)
{
  struct mro_db *db = ctx;
  db->get_model_calls++;
  for (int i = 0; i < db->n_models; i++) {
    if (strcmp(db->models[i].name, name) == 0) {
      *out = db->models[i];
      return 0;
    }
  }
  return -1;
}

static int mro_fail(void *ctx, ...)
{
  (void)ctx;
  return -1;
}

static pg_ops_t build_mro_ops(struct mro_db *db)
{
  pg_ops_t ops;
  memset(&ops, 0, sizeof ops);
  ops.ctx = db;
  ops.get_model = mro_get_model;
  ops.get_key_by_hash = (int (*)(void *, const char *, key_rec_t *))mro_fail;
  ops.list_models = (int (*)(void *, model_rec_t *, int, int *))mro_fail;
  ops.create_key = (int (*)(void *, const key_rec_t *, long *))mro_fail;
  ops.update_key = (int (*)(void *, const key_rec_t *, int))mro_fail;
  ops.revoke_key = (int (*)(void *, long))mro_fail;
  ops.create_model = (int (*)(void *, const model_rec_t *))mro_fail;
  ops.update_model = (int (*)(void *, const model_rec_t *, int))mro_fail;
  ops.delete_model = (int (*)(void *, const char *))mro_fail;
  ops.flush_usage = (int (*)(void *, const usage_row_t *, int))mro_fail;
  ops.query_usage =
      (int (*)(void *, long, const char *, time_t, time_t, usage_row_t *, int,
               int *))mro_fail;
  return ops;
}

static pg_store_t *open_mro_store(struct mro_db *db)
{
  pg_ops_t ops = build_mro_ops(db);
  return pg_store_open("unused", &ops);
}

TEST_CASE(test_model_router_env_key)
{
  struct mro_db db;
  memset(&db, 0, sizeof db);
  db.n_models = 1;
  strcpy(db.models[0].name, "gpt-4o");
  strcpy(db.models[0].provider, "openai");
  strcpy(db.models[0].endpoint, "http://127.0.0.1:9999/v1");
  strcpy(db.models[0].upstream_key_ref, "env:TEST_UPSTREAM_KEY");
  strcpy(db.models[0].default_params_json, "{\"model\":\"gpt-4o\"}");
  db.models[0].enabled = 1;

  setenv("TEST_UPSTREAM_KEY", "sk-test-123", 1);
  pg_store_t *ps = open_mro_store(&db);
  TEST_ASSERT(ps != NULL, "store open");
  model_router_t *mr = model_router_new(ps, NULL);
  TEST_ASSERT(mr != NULL, "router new");

  model_rec_t out;
  int calls_before = db.get_model_calls;
  TEST_ASSERT(model_router_resolve(mr, "gpt-4o", &out) == 0, "resolve");
  TEST_ASSERT(strcmp(out.upstream_key, "sk-test-123") == 0, "env key filled");
  TEST_ASSERT(strcmp(out.endpoint, "http://127.0.0.1:9999/v1") == 0,
              "endpoint");
  int calls_after = db.get_model_calls;
  TEST_ASSERT(calls_after == calls_before + 1, "first resolve hit store");

  /* second resolve is cached */
  model_rec_t out2;
  TEST_ASSERT(model_router_resolve(mr, "gpt-4o", &out2) == 0, "cached resolve");
  TEST_ASSERT(db.get_model_calls == calls_after, "cache hit, no store call");

  model_router_invalidate(mr, "gpt-4o");
  model_router_resolve(mr, "gpt-4o", &out2);
  TEST_ASSERT(db.get_model_calls == calls_after + 1, "invalidation re-reads");

  /* unknown model */
  TEST_ASSERT(model_router_resolve(mr, "nope", &out2) == -1, "unknown model");

  model_router_free(mr);
  pg_store_close(ps);
  unsetenv("TEST_UPSTREAM_KEY");
}

TEST_CASE(test_model_router_missing_env_key)
{
  struct mro_db db;
  memset(&db, 0, sizeof db);
  db.n_models = 1;
  strcpy(db.models[0].name, "x");
  strcpy(db.models[0].provider, "openai");
  strcpy(db.models[0].endpoint, "http://127.0.0.1:9999");
  strcpy(db.models[0].upstream_key_ref, "env:DOES_NOT_EXIST_KEY");
  db.models[0].enabled = 1;

  pg_store_t *ps = open_mro_store(&db);
  TEST_ASSERT(ps != NULL, "store open");
  model_router_t *mr = model_router_new(ps, NULL);
  TEST_ASSERT(mr != NULL, "router new");

  model_rec_t out;
  TEST_ASSERT(model_router_resolve(mr, "x", &out) == -1,
              "missing env key → -1");
  model_router_free(mr);
  pg_store_close(ps);
}
