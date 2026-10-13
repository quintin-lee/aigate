/** @file test_admin_ui.c
 *  @brief Unit tests for admin web UI embedded delivery.
 */
#include "run_tests.h"
#include "admin_ui.h"
#include <string.h>

TEST_CASE(admin_ui_content)
{
    size_t      len = 0;
    const char* html = admin_ui_get_html(&len);

    TEST_ASSERT(html != NULL, "admin_ui_get_html returned NULL");
    TEST_ASSERT(len > 1000, "admin_ui HTML too short: %zu bytes", len);
    TEST_ASSERT(strstr(html, "<!DOCTYPE html>") != NULL, "missing doctype");
    TEST_ASSERT(strstr(html, "aigate — AI 网关控制台") != NULL ||
                    strstr(html, "aigate — AI Gateway Console") != NULL,
                "missing title");
    TEST_ASSERT(strstr(html, "tab-overview") != NULL, "missing tab-overview");
    TEST_ASSERT(strstr(html, "tab-models") != NULL, "missing tab-models");
    TEST_ASSERT(strstr(html, "tab-keys") != NULL, "missing tab-keys");
    TEST_ASSERT(strstr(html, "tab-usage") != NULL, "missing tab-usage");
    TEST_ASSERT(strstr(html, "tab-playground") != NULL, "missing tab-playground");
    TEST_ASSERT(strstr(html, "tab-metrics") != NULL, "missing tab-metrics");
    TEST_ASSERT(strstr(html, "tab-groups") != NULL, "missing tab-groups");
    TEST_ASSERT(strstr(html, "tab-cost") != NULL, "missing tab-cost");
    TEST_ASSERT(strstr(html, "data-tab=\"shadow\"") != NULL, "missing shadow nav button");
    TEST_ASSERT(strstr(html, "id=\"tab-shadow\"") != NULL, "missing tab-shadow pane");
    TEST_ASSERT(strstr(html, "id=\"shadowKpiTotal\"") != NULL, "missing shadowKpiTotal");
    TEST_ASSERT(strstr(html, "id=\"shadowKpiLatency\"") != NULL, "missing shadowKpiLatency");
    TEST_ASSERT(strstr(html, "id=\"shadowKpiCost\"") != NULL, "missing shadowKpiCost");
    TEST_ASSERT(strstr(html, "id=\"shadowKpiHealth\"") != NULL, "missing shadowKpiHealth");
    TEST_ASSERT(strstr(html, "id=\"shadowRuleModal\"") != NULL, "missing shadowRuleModal");
    TEST_ASSERT(strstr(html, "id=\"shadowDiffModal\"") != NULL, "missing shadowDiffModal");
    TEST_ASSERT(strstr(html, "data-tab=\"compressor\"") != NULL, "missing compressor nav button");
    TEST_ASSERT(strstr(html, "id=\"tab-compressor\"") != NULL, "missing tab-compressor pane");
    TEST_ASSERT(strstr(html, "id=\"compressorKpiSavedTokens\"") != NULL,
                "missing compressorKpiSavedTokens");
    TEST_ASSERT(strstr(html, "id=\"compressorKpiSavings\"") != NULL,
                "missing compressorKpiSavings");
    TEST_ASSERT(strstr(html, "id=\"compressorKpiRatio\"") != NULL, "missing compressorKpiRatio");
    TEST_ASSERT(strstr(html, "id=\"compressorKpiLatency\"") != NULL,
                "missing compressorKpiLatency");
    TEST_ASSERT(strstr(html, "id=\"compressorRuleModal\"") != NULL, "missing compressorRuleModal");
    TEST_ASSERT(strstr(html, "id=\"compressorDiffModal\"") != NULL, "missing compressorDiffModal");
    TEST_ASSERT(strstr(html, "data-tab=\"cache-optimizer\"") != NULL,
                "missing cache-optimizer tab button");
    TEST_ASSERT(strstr(html, "id=\"tab-cache-optimizer\"") != NULL,
                "missing tab-cache-optimizer section");
    TEST_ASSERT(strstr(html, "id=\"cacheOptKpiHitRate\"") != NULL, "missing cacheOptKpiHitRate");
    TEST_ASSERT(strstr(html, "id=\"cacheOptKpiTokens\"") != NULL, "missing cacheOptKpiTokens");
    TEST_ASSERT(strstr(html, "id=\"cacheOptKpiSavings\"") != NULL, "missing cacheOptKpiSavings");
    TEST_ASSERT(strstr(html, "id=\"cacheOptKpiLatency\"") != NULL, "missing cacheOptKpiLatency");
    TEST_ASSERT(strstr(html, "id=\"cacheOptRuleModal\"") != NULL, "missing cacheOptRuleModal");
    TEST_ASSERT(strstr(html, "id=\"cacheOptDetailModal\"") != NULL, "missing cacheOptDetailModal");
    TEST_ASSERT(strstr(html, "id=\"mFormSupportsReasoning\"") != NULL,
                "missing mFormSupportsReasoning");
    TEST_ASSERT(strstr(html, "id=\"mFormThinkingBudget\"") != NULL, "missing mFormThinkingBudget");
    TEST_ASSERT(strstr(html, "id=\"mFormTagEmulationDefault\"") != NULL,
                "missing mFormTagEmulationDefault");
    TEST_ASSERT(strstr(html, "id=\"mFormPricingReasoning\"") != NULL,
                "missing mFormPricingReasoning");
    TEST_ASSERT(strstr(html, "id=\"kFormReasoningQuota\"") != NULL, "missing kFormReasoningQuota");
    TEST_ASSERT(strstr(html, "id=\"gFormDailyReasoningQuota\"") != NULL,
                "missing gFormDailyReasoningQuota");
    TEST_ASSERT(strstr(html, "id=\"playReasoningFormat\"") != NULL, "missing playReasoningFormat");
    TEST_ASSERT(strstr(html, "id=\"playDegradedBanner\"") != NULL, "missing playDegradedBanner");
    TEST_ASSERT(strstr(html, "id=\"playStatReasoningTokens\"") != NULL,
                "missing playStatReasoningTokens");
    TEST_ASSERT(strstr(html, "aigate_tokens_reasoning_total") != NULL,
                "missing aigate_tokens_reasoning_total");
    TEST_ASSERT(strstr(html, "reasoning-accordion") != NULL, "missing reasoning-accordion");
    TEST_ASSERT(strstr(html, "thinking-accordion") != NULL, "missing thinking-accordion");
    TEST_ASSERT(strstr(html, "X-Aigate-Reasoning-Degraded") != NULL,
                "missing X-Aigate-Reasoning-Degraded");
    TEST_ASSERT(strstr(html, "id=\"reasoning-box\"") != NULL, "missing reasoning-box");
    TEST_ASSERT(strstr(html, "badge-purple") != NULL, "missing badge-purple");
    TEST_ASSERT(strstr(html, "id=\"tab-plugins\"") != NULL, "missing tab-plugins");
    TEST_ASSERT(strstr(html, "id=\"monacoEditorContainer\"") != NULL,
                "missing monacoEditorContainer");
    TEST_ASSERT(strstr(html, "id=\"monacoDiffContainer\"") != NULL, "missing monacoDiffContainer");
    TEST_ASSERT(strstr(html, "id=\"ideStatusBar\"") != NULL, "missing ideStatusBar");
    TEST_ASSERT(strstr(html, "id=\"ideLintStatus\"") != NULL, "missing ideLintStatus");
    TEST_ASSERT(strstr(html, "id=\"monacoFallbackBadge\"") != NULL, "missing monacoFallbackBadge");
    TEST_ASSERT(strstr(html, "id=\"ideConsoleLogs\"") != NULL, "missing ideConsoleLogs");
}
