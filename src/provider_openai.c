/** @file provider_openai.c
 *  @brief OpenAI-compatible request builder (see provider_openai.h). */
#include "provider_openai.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

int provider_openai_supports(const char *provider)
{
  return provider != NULL &&
         (strcmp(provider, "openai") == 0 || strcmp(provider, "ollama") == 0 ||
          strcmp(provider, "azure") == 0);
}

int provider_openai_build(const model_rec_t *route, const char *in_body,
                          char *url_out, size_t url_cap, char **out_body,
                          size_t *out_body_len)
{
  json_t *req = json_object();
  json_t *defaults = NULL;
  const char *base = route->endpoint;
  const char *path = "";
  char merged_url[1024];

  /* parse inputs; a malformed request body is the caller's problem upstream —
   * we merge only what parses cleanly. */
  if (in_body != NULL && in_body[0] != '\0') {
    json_t *parsed = json_loads(in_body, 0, NULL);
    if (parsed != NULL) {
      json_object_update(req, parsed);
      json_decref(parsed);
    }
  }
  if (route->default_params_json[0] != '\0') {
    defaults = json_loads(route->default_params_json, 0, NULL);
    if (defaults == NULL) {
      AIGATE_LOG_WARN("bad default_params for model %s", route->name);
    } else {
      /* defaults first so the request body wins on conflict */
      json_t *base = json_object();
      json_object_update(base, defaults);
      json_object_update(base, req);
      json_decref(req);
      json_decref(defaults);
      req = base;
    }
  }

  /* path: use "model" → standard /chat/completions only if body lacks a
   * provider-specific path hint; the gateway routes by endpoint, so keep it
   * simple: caller passes the path via default_params "path" when needed. */
  json_t *jpath = json_object_get(req, "path");
  if (jpath != NULL && json_is_string(jpath)) {
    path = json_string_value(jpath);
    json_object_del(req, "path");
  }

  char *url_buf = malloc(url_cap);
  if (url_buf == NULL || out_body == NULL) {
    json_decref(req);
    free(url_buf);
    return -1;
  }

  if (strcmp(route->provider, "azure") == 0) {
    json_t *jver = json_object_get(req, "api-version");
    const char *ver =
        (jver != NULL && json_is_string(jver)) ? json_string_value(jver) : "";
    if (ver[0] != '\0') {
      snprintf(url_buf, url_cap, "%s%s?api-version=%s", base, path, ver);
      json_object_del(req, "api-version");
    } else {
      snprintf(url_buf, url_cap, "%s%s", base, path);
    }
  } else {
    snprintf(url_buf, url_cap, "%s%s", base, path);
  }
  snprintf(merged_url, sizeof merged_url, "%s", url_buf);

  char *packed = json_dumps(req, JSON_COMPACT);
  json_decref(req);
  if (packed == NULL) {
    free(url_buf);
    return -1;
  }
  snprintf(url_out, url_cap, "%s", merged_url);
  *out_body = packed;
  if (out_body_len != NULL)
    *out_body_len = strlen(packed);
  free(url_buf);
  return 0;
}
