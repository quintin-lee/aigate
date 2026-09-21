/** @file provider_adapter.c
 *  @brief Provider adapter registry implementation (Plan 3, Task 1).
 */
#include "provider_adapter.h"
#include <string.h>

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
