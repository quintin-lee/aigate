/**
 * @file test_admin_threat_and_ban_api.c
 * @brief Unit tests for /admin/v1/bans and /admin/v1/threat-whitelists endpoints.
 */
#include "admin_api.h"
#include "core/aigate_core.h"
#include "policy/ip_ban_table.h"
#include "policy/threat_whitelist.h"
#include "run_tests.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_admin_bans_crud)
{
    aigate_core ac;
    memset(&ac, 0, sizeof(ac));
    ac.ip_ban_tbl = ip_ban_table_create();
    TEST_ASSERT(ac.ip_ban_tbl != NULL, "ip_ban_table_create failed");

    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    adm.ac = &ac;
    /* SHA-256 of "secret" */
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. GET /admin/v1/bans -> empty */
    int rc = admin_dispatch(
        &adm, "/admin/v1/bans", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET /admin/v1/bans returns 200");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "valid json");
    json_t* arr = json_object_get(res, "bans");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 0, "bans list empty");
    json_decref(res);

    /* 2. POST /admin/v1/bans with invalid body */
    rc = admin_dispatch(
        &adm, "/admin/v1/bans", "POST", NULL, "secret", "not json", 8, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 400, "invalid json returns 400");
    free(body);
    body = NULL;

    /* 3. POST /admin/v1/bans without ip */
    rc = admin_dispatch(
        &adm, "/admin/v1/bans", "POST", NULL, "secret", "{}", 2, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 400, "missing ip returns 400");
    free(body);
    body = NULL;

    /* 4. POST /admin/v1/bans -> ban IP */
    const char* ban_req =
        "{\"ip\":\"198.51.100.22\",\"reason\":\"manual_operator_ban\",\"ttl_seconds\":3600}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/bans",
                        "POST",
                        NULL,
                        "secret",
                        ban_req,
                        strlen(ban_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "POST /admin/v1/bans returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "valid json");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(res, "status")), "banned") == 0,
                "status is banned");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(res, "ip")), "198.51.100.22") == 0,
                "ip matches");
    json_decref(res);

    /* 5. GET /admin/v1/bans -> 1 banned IP */
    rc = admin_dispatch(
        &adm, "/admin/v1/bans", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET /admin/v1/bans returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    arr = json_object_get(res, "bans");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 1, "bans list has 1 item");
    json_t* item = json_array_get(arr, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item, "ip")), "198.51.100.22") == 0,
                "ip matches in list");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item, "reason")), "manual_operator_ban") ==
                    0,
                "reason matches");
    TEST_ASSERT(json_integer_value(json_object_get(item, "remaining_ttl_s")) > 0,
                "remaining_ttl_s > 0");
    json_decref(res);

    /* 6. DELETE /admin/v1/bans/198.51.100.22 -> unban */
    rc = admin_dispatch(&adm,
                        "/admin/v1/bans/198.51.100.22",
                        "DELETE",
                        NULL,
                        "secret",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "DELETE returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(strcmp(json_string_value(json_object_get(res, "status")), "unbanned") == 0,
                "status is unbanned");
    json_decref(res);

    /* 7. DELETE /admin/v1/bans/198.51.100.22 again -> 404 */
    rc = admin_dispatch(&adm,
                        "/admin/v1/bans/198.51.100.22",
                        "DELETE",
                        NULL,
                        "secret",
                        NULL,
                        0,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 404, "DELETE already unbanned returns 404");
    free(body);
    body = NULL;

    /* 8. GET /admin/v1/bans -> empty again */
    rc = admin_dispatch(
        &adm, "/admin/v1/bans", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET /admin/v1/bans returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    arr = json_object_get(res, "bans");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 0, "bans list empty after unban");
    json_decref(res);

    ip_ban_table_destroy(ac.ip_ban_tbl);
}

TEST_CASE(test_admin_threat_whitelists_crud)
{
    aigate_core ac;
    memset(&ac, 0, sizeof(ac));
    ac.threat_whitelist = threat_whitelist_create();
    TEST_ASSERT(ac.threat_whitelist != NULL, "threat_whitelist_create failed");

    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    adm.ac = &ac;
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. GET /admin/v1/threat-whitelists -> empty */
    int rc = admin_dispatch(
        &adm, "/admin/v1/threat-whitelists", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET returns 200");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "valid json");
    json_t* arr = json_object_get(res, "whitelists");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 0, "whitelists empty");
    json_decref(res);

    /* 2. POST /admin/v1/threat-whitelists with invalid json */
    rc = admin_dispatch(&adm,
                        "/admin/v1/threat-whitelists",
                        "POST",
                        NULL,
                        "secret",
                        "bad json",
                        8,
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 400, "invalid json returns 400");
    free(body);
    body = NULL;

    /* 3. POST /admin/v1/threat-whitelists -> create rule */
    const char* create_req =
        "{\"name\":\"sec_eval\",\"match_key_id\":8899,\"match_model\":\"gpt-4*\","
        "\"bypass_rule_tag\":\"instruction_override\",\"reason\":\"authorized red team\","
        "\"enabled\":true}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/threat-whitelists",
                        "POST",
                        NULL,
                        "secret",
                        create_req,
                        strlen(create_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 201, "POST returns 201");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "valid json");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(res, "status")), "created") == 0,
                "status created");
    int64_t rule_id = (int64_t)json_integer_value(json_object_get(res, "rule_id"));
    TEST_ASSERT(rule_id > 0, "valid rule_id > 0");
    json_decref(res);

    /* 4. GET /admin/v1/threat-whitelists -> 1 rule */
    rc = admin_dispatch(
        &adm, "/admin/v1/threat-whitelists", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    arr = json_object_get(res, "whitelists");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 1, "whitelists has 1 item");
    json_t* item = json_array_get(arr, 0);
    TEST_ASSERT(json_integer_value(json_object_get(item, "rule_id")) == rule_id, "rule_id match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item, "name")), "sec_eval") == 0,
                "name match");
    TEST_ASSERT(json_integer_value(json_object_get(item, "match_key_id")) == 8899,
                "match_key_id match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item, "match_model")), "gpt-4*") == 0,
                "match_model match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(item, "bypass_rule_tag")),
                       "instruction_override") == 0,
                "bypass_rule_tag match");
    TEST_ASSERT(json_is_true(json_object_get(item, "enabled")), "enabled is true");
    json_decref(res);

    /* 5. DELETE /admin/v1/threat-whitelists/<rule_id> */
    char del_uri[128];
    snprintf(del_uri, sizeof(del_uri), "/admin/v1/threat-whitelists/%ld", (long)rule_id);
    rc = admin_dispatch(&adm, del_uri, "DELETE", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "DELETE returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(strcmp(json_string_value(json_object_get(res, "status")), "deleted") == 0,
                "status deleted");
    json_decref(res);

    /* 6. GET /admin/v1/threat-whitelists -> empty */
    rc = admin_dispatch(
        &adm, "/admin/v1/threat-whitelists", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    arr = json_object_get(res, "whitelists");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 0, "whitelists empty after delete");
    json_decref(res);

    threat_whitelist_destroy(ac.threat_whitelist);
}
