/** @file test_admin_ui.c
 *  @brief Unit tests for admin web UI embedded delivery.
 */
#include "run_tests.h"
#include "admin_ui.h"
#include <string.h>

TEST_CASE(admin_ui_content)
{
    size_t len = 0;
    const char* html = admin_ui_get_html(&len);

    TEST_ASSERT(html != NULL, "admin_ui_get_html returned NULL");
    TEST_ASSERT(len > 1000, "admin_ui HTML too short: %zu bytes", len);
    TEST_ASSERT(strstr(html, "<!DOCTYPE html>") != NULL, "missing doctype");
    TEST_ASSERT(strstr(html, "aigate — AI Gateway Console") != NULL, "missing title");
    TEST_ASSERT(strstr(html, "tab-overview") != NULL, "missing tab-overview");
    TEST_ASSERT(strstr(html, "tab-models") != NULL, "missing tab-models");
    TEST_ASSERT(strstr(html, "tab-keys") != NULL, "missing tab-keys");
    TEST_ASSERT(strstr(html, "tab-usage") != NULL, "missing tab-usage");
    TEST_ASSERT(strstr(html, "tab-playground") != NULL, "missing tab-playground");
    TEST_ASSERT(strstr(html, "tab-metrics") != NULL, "missing tab-metrics");
}
