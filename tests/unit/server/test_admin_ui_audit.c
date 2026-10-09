/** @file test_admin_ui_audit.c
 *  @brief Unit test for embedded admin UI audit forensic drawer and live stream components.
 */
#include "admin_ui.h"
#include "run_tests.h"
#include <string.h>

TEST_CASE(test_admin_ui_contains_audit_forensic_drawer)
{
    size_t      len = 0;
    const char* html = admin_ui_get_html(&len);
    TEST_ASSERT(html != NULL, "admin_ui_get_html returned NULL");
    TEST_ASSERT(strstr(html, "id=\"tab-audit\"") != NULL, "missing tab-audit");
    TEST_ASSERT(strstr(html, "id=\"auditForensicDrawer\"") != NULL, "missing auditForensicDrawer");
    TEST_ASSERT(strstr(html, "toggleAuditLiveStream") != NULL, "missing toggleAuditLiveStream");
    TEST_ASSERT(strstr(html, "exportAuditNdjson") != NULL, "missing exportAuditNdjson");
}
