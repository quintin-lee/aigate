/** @file provider_openai.c
 *  @brief OpenAI-compatible request builder (see provider_openai.h). */
#include "provider_openai.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

int
provider_openai_supports(const char* provider)
{
    return provider != NULL && (strcmp(provider, "openai") == 0 ||
                                strcmp(provider, "ollama") == 0 || strcmp(provider, "azure") == 0);
}

int
provider_openai_build(const model_rec_t* route,
                      const char*        up_path,
                      const char*        in_body,
                      char*              url_out,
                      size_t             url_cap,
                      char**             out_body,
                      size_t*            out_body_len)
{
    /* start from the request body object (may be empty) */
    json_t* req = json_object();
    if (in_body != NULL && in_body[0] != '\0') {
        json_t* parsed = json_loads(in_body, 0, NULL);
        if (parsed == NULL) {
            /* malformed request JSON: treat as empty; the upstream will reject it */
            AIGATE_LOG_WARN("request body for model %s is not valid JSON", route->name);
        } else {
            json_object_update(req, parsed);
            json_decref(parsed);
        }
    }
    /* merge default_params under the request body (request wins on conflict) */
    if (route->default_params_json[0] != '\0') {
        json_t* defaults = json_loads(route->default_params_json, 0, NULL);
        if (defaults == NULL) {
            AIGATE_LOG_WARN("bad default_params for model %s", route->name);
        } else {
            json_t* merged = json_object();
            json_object_update(merged, defaults); /* defaults first */
            json_object_update(merged, req);      /* request wins */
            json_decref(req);
            json_decref(defaults);
            req = merged;
        }
    }

    /* URL: endpoint + up_path; for azure, append ?api-version= (from params). */
    const char* ver = "";
    if (strcmp(route->provider, "azure") == 0) {
        json_t* jv = json_object_get(req, "api-version");
        if (jv != NULL && json_is_string(jv)) {
            ver = json_string_value(jv);
        }
    }

    char packed_url[1024];
    if (ver[0] != '\0') {
        snprintf(packed_url,
                 sizeof packed_url,
                 "%s%s?api-version=%s",
                 route->endpoint,
                 up_path != NULL ? up_path : "",
                 ver);
        json_object_del(req, "api-version");
    } else {
        snprintf(
            packed_url, sizeof packed_url, "%s%s", route->endpoint, up_path != NULL ? up_path : "");
    }
    snprintf(url_out, url_cap, "%s", packed_url);

    char* packed = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (packed == NULL) {
        return -1;
    }
    *out_body = packed;
    if (out_body_len != NULL) {
        *out_body_len = strlen(packed);
    }
    return 0;
}
