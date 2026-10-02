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
}
