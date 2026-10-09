/**
 * @file test_admin_ui_watermark_and_chain.c
 * @brief Unit test verifying watermark forensic decoder and audit hash chain verifier UI presence.
 */

#include "admin_ui.h"
#include "run_tests.h"
#include <string.h>

TEST_CASE(test_admin_ui_watermark_and_chain_elements)
{
    size_t      len = 0;
    const char* html = admin_ui_get_html(&len);
    TEST_ASSERT(html != NULL, "Embedded admin UI html should not be NULL");
    TEST_ASSERT(len > 0, "Embedded admin UI html length should be > 0");

    TEST_ASSERT(strstr(html, "watermark-input") != NULL,
                "Embedded admin UI should contain watermark-input element");
    TEST_ASSERT(strstr(html, "btn-decode-watermark") != NULL,
                "Embedded admin UI should contain btn-decode-watermark element");
    TEST_ASSERT(strstr(html, "watermark-result") != NULL,
                "Embedded admin UI should contain watermark-result element");
    TEST_ASSERT(strstr(html, "btn-verify-chain") != NULL,
                "Embedded admin UI should contain btn-verify-chain element");
    TEST_ASSERT(strstr(html, "chain-status-badge") != NULL,
                "Embedded admin UI should contain chain-status-badge element");
}
