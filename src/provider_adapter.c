/** @file provider_adapter.c
 *  @brief Provider adapter registry implementation (Plan 3, Task 1).
 */
#include "provider_adapter.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
extern const provider_adapter_t g_provider_openai;
extern const provider_adapter_t g_provider_anthropic;
extern const provider_adapter_t g_provider_gemini;

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
provider_probe_plan(const char* provider_type,
                    const char* endpoint,
                    provider_probe_plan_t* out)
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
