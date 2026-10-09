/** @file test_admin_audit_and_sla_api.c
 *  @brief Unit tests for /admin/v1/audit and /admin/v1/models/sla API endpoints.
 */
#include "admin_api.h"
#include "run_tests.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_admin_audit_and_sla_endpoints)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    /* SHA-256 of "secret" */
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. GET /admin/v1/audit/events */
    int rc = admin_dispatch(&adm,
                            "/admin/v1/audit/events?limit=10",
                            "GET",
                            NULL,
                            "secret",
                            NULL,
                            0,
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0, "audit events rc should be 0, got %d", rc);
    TEST_ASSERT(status == 200, "audit events status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "audit events body should not be null");
    free(body);
    body = NULL;

    /* 2. GET /admin/v1/audit/violations */
    rc = admin_dispatch(&adm,
                        "/admin/v1/audit/violations?limit=10",
                        "GET",
                        NULL,
                        "secret",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0, "audit violations rc should be 0, got %d", rc);
    TEST_ASSERT(status == 200, "audit violations status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "audit violations body should not be null");
    free(body);
    body = NULL;

    /* 3. GET /admin/v1/models/sla */
    rc = admin_dispatch(
        &adm, "/admin/v1/models/sla", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0, "models sla rc should be 0, got %d", rc);
    TEST_ASSERT(status == 200, "models sla status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "models sla body should not be null");
    free(body);
    body = NULL;

    /* 4. POST /admin/v1/models/deepseek-r1/sla/override */
    const char* post_body = "{\"action\":\"degrade\"}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/models/deepseek-r1/sla/override",
                        "POST",
                        NULL,
                        "secret",
                        post_body,
                        strlen(post_body),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0, "override rc should be 0, got %d", rc);
    TEST_ASSERT(status == 200, "override status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "override body should not be null");
    free(body);
}
