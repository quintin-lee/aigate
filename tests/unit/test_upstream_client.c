/** @file test_upstream_client.c
 *  @brief upstream_call against the in-process mock upstream. */
#include "run_tests.h"
#include "upstream_client.h"
#include "mock_upstream.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_upstream_200_roundtrip)
{
  const char *base = mock_upstream_start();
  TEST_ASSERT(base != NULL, "mock started");

  model_rec_t route;
  memset(&route, 0, sizeof route);
  strcpy(route.provider, "openai");
  strcpy(route.endpoint, base); /* + path "/chat" */
  strcpy(route.upstream_key, "sk-mock");

  int status = 0;
  char *body = NULL;
  size_t blen = 0;
  int rc = upstream_call(&route, "/chat",
                         "{\"model\":\"gpt-4o\",\"messages\":[]}", 0, 5000,
                         &status, &body, &blen);
  TEST_ASSERT(rc == 0, "transport ok, rc=%d", rc);
  TEST_ASSERT(status == 200, "status 200, got %d", status);
  TEST_ASSERT(body != NULL && blen > 0, "body present");

  json_t *j = json_loads(body, 0, NULL);
  TEST_ASSERT(j != NULL, "body is JSON: %s", body ? body : "(null)");
  json_t *usage = json_object_get(j, "usage");
  TEST_ASSERT(usage != NULL, "usage present");
  TEST_ASSERT(json_integer_value(json_object_get(usage, "prompt_tokens")) == 7,
              "prompt_tokens 7");
  TEST_ASSERT(json_integer_value(json_object_get(usage, "completion_tokens")) == 11,
              "completion_tokens 11");
  json_decref(j);
  free(body);
}

TEST_CASE(test_upstream_500_passthrough)
{
  const char *base = mock_upstream_start();
  TEST_ASSERT(base != NULL, "mock started");

  model_rec_t route;
  memset(&route, 0, sizeof route);
  strcpy(route.provider, "openai");
  strcpy(route.endpoint, base);

  int status = 0;
  char *body = NULL;
  size_t blen = 0;
  int rc = upstream_call(&route, "/fail", "{}", 0, 5000, &status, &body, &blen);
  TEST_ASSERT(rc == 0, "transport ok even on 5xx, rc=%d", rc);
  TEST_ASSERT(status == 500, "status 500, got %d", status);
  free(body);
}

TEST_CASE(test_upstream_timeout)
{
  const char *base = mock_upstream_start();
  TEST_ASSERT(base != NULL, "mock started");

  model_rec_t route;
  memset(&route, 0, sizeof route);
  strcpy(route.provider, "openai");
  strcpy(route.endpoint, base);

  int status = 0;
  char *body = NULL;
  size_t blen = 0;
  int rc = upstream_call(&route, "/slow", "{}", 0, 200, &status, &body, &blen);
  TEST_ASSERT(rc == -110, "timeout → -110, got %d", rc);
  free(body);
  mock_upstream_stop();
}
