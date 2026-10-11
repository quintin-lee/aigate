/** @file provider_adapter.c
 *  @brief Provider adapter registry implementation (Plan 3, Task 1).
 */
#include "provider_adapter.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
/** @brief Supplier vtable externs (defined in each provider_*.c, documented in the matching header). */
extern const provider_adapter_t g_provider_openai;
/** @copydoc g_provider_openai */
extern const provider_adapter_t g_provider_anthropic;
/** @copydoc g_provider_openai */
extern const provider_adapter_t g_provider_gemini;

/** Supplier adapter registry (NULL-terminated, matched in supports(provider) order). */
static const provider_adapter_t* s_adapters[] = {
    &g_provider_openai, &g_provider_anthropic, &g_provider_gemini, NULL};

const provider_adapter_t*
provider_find(const char* provider)
{
    if (provider == NULL || provider[0] == '\0') {
        return NULL;
    }
    for (int i = 0; s_adapters[i] != NULL; i++) {
        if (s_adapters[i]->supports != NULL && s_adapters[i]->supports(provider)) {
            return s_adapters[i];
        }
    }
    return NULL;
}

int
provider_probe_plan(const char* provider_type, const char* endpoint, provider_probe_plan_t* out)
{
    const provider_adapter_t* adp = provider_find(provider_type);
    if (adp == NULL || endpoint == NULL || endpoint[0] == '\0' || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    size_t elen = strlen(endpoint);

    int wrote;
    if (strcasecmp(adp->name, "anthropic") == 0) {
        /* Mirror the /messages URL rule (provider_anthropic.c:31-39). */
        if (elen >= 3 && strcmp(endpoint + elen - 3, "/v1") == 0) {
            wrote = snprintf(out->url, sizeof out->url, "%s/models", endpoint);
        } else if (elen >= 4 && strcmp(endpoint + elen - 4, "/v1/") == 0) {
            wrote = snprintf(out->url, sizeof out->url, "%smodels", endpoint);
        } else {
            wrote = snprintf(out->url, sizeof out->url, "%s/v1/models", endpoint);
        }
        snprintf(out->auth_header, sizeof out->auth_header, "x-api-key");
        out->bearer = 0;
        snprintf(out->extra_header, sizeof out->extra_header, "anthropic-version");
    } else if (strcasecmp(adp->name, "gemini") == 0) {
        wrote = snprintf(out->url, sizeof out->url, "%s/v1beta/models", endpoint);
        snprintf(out->auth_header, sizeof out->auth_header, "x-goog-api-key");
        out->bearer = 0;
    } else { /* openai family: openai/ollama/azure/deepseek/siliconflow/vllm */
        wrote = snprintf(out->url, sizeof out->url, "%s/models", endpoint);
        snprintf(out->auth_header, sizeof out->auth_header, "Authorization");
        out->bearer = 1;
    }
    if (wrote < 0 || (size_t)wrote >= sizeof out->url) {
        return -1;
    }
    return 0;
}

int
parse_reasoning_config(json_t* req_body, const model_rec_t* route, reasoning_config_t* out_cfg)
{
    if (out_cfg == NULL) {
        return -1;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    if (req_body != NULL && json_is_object(req_body)) {
        /* 1. Explicit Anthropic thinking object */
        json_t* jth = json_object_get(req_body, "thinking");
        if (jth != NULL && json_is_object(jth)) {
            json_t* jtype = json_object_get(jth, "type");
            json_t* jb = json_object_get(jth, "budget_tokens");
            if (jtype != NULL && json_is_string(jtype) && strcmp(json_string_value(jtype), "enabled") == 0) {
                out_cfg->enabled = true;
                if (jb != NULL && json_is_integer(jb)) {
                    out_cfg->budget_tokens = json_integer_value(jb);
                }
                return 0;
            }
        }

        /* 2. max_thinking_tokens parameter */
        json_t* jmtt = json_object_get(req_body, "max_thinking_tokens");
        if (jmtt != NULL && json_is_integer(jmtt)) {
            out_cfg->enabled = true;
            out_cfg->budget_tokens = json_integer_value(jmtt);
            return 0;
        }

        /* 3. OpenAI reasoning_effort parameter */
        json_t* jeff = json_object_get(req_body, "reasoning_effort");
        if (jeff != NULL && json_is_string(jeff)) {
            const char* eff = json_string_value(jeff);
            snprintf(out_cfg->effort, sizeof(out_cfg->effort), "%s", eff);
            out_cfg->enabled = true;
            if (strcmp(eff, "low") == 0) {
                out_cfg->budget_tokens = 1024;
            } else if (strcmp(eff, "medium") == 0) {
                out_cfg->budget_tokens = 4096;
            } else if (strcmp(eff, "high") == 0) {
                out_cfg->budget_tokens = 16384;
            } else if (strcmp(eff, "none") == 0) {
                out_cfg->enabled = false;
                out_cfg->budget_tokens = 0;
            } else {
                out_cfg->budget_tokens = 4096;
            }
            return 0;
        }
    }

    /* 4. Model route fallback */
    if (route != NULL && route->supports_reasoning && route->default_thinking_budget > 0) {
        out_cfg->enabled = true;
        out_cfg->budget_tokens = route->default_thinking_budget;
        return 0;
    }

    return 0;
}
