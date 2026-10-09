/**
 * @file test_admin_watermark_and_chain_api.c
 * @brief Unit tests for /admin/v1/watermark/decode and /admin/v1/audit/chain/verify.
 */
#include "admin_api.h"
#include "policy/watermark_engine.h"
#include "run_tests.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_admin_watermark_decode_endpoint)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    /* SHA-256 of "secret" */
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    /* 1. 生成带有水印的测试文本 */
    watermark_payload_t wp;
    wp.timestamp = 1791552000;
    wp.key_id = 5566;
    wp.short_trace = 0x123456789ABCDEF0ULL;
    wp.crc_valid = false;

    const char* base_txt = "Confidential report for internal eyes only.";
    size_t      wm_len = 0;
    char*       wm_txt = watermark_inject(base_txt, strlen(base_txt), &wp, &wm_len);
    TEST_ASSERT(wm_txt != NULL, "watermark_inject failed");

    json_t* req_obj = json_object();
    json_object_set_new(req_obj, "text", json_string(wm_txt));
    char* req_body = json_dumps(req_obj, JSON_COMPACT);
    json_decref(req_obj);
    free(wm_txt);

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    int rc = admin_dispatch(&adm,
                            "/admin/v1/watermark/decode",
                            "POST",
                            NULL,
                            "secret",
                            req_body,
                            strlen(req_body),
                            &status,
                            &body,
                            &len);
    free(req_body);

    TEST_ASSERT(rc == 0, "admin_dispatch should return 0, got %d", rc);
    TEST_ASSERT(status == 200, "status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "response body should not be null");

    json_t* res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "response must be valid json");
    TEST_ASSERT(json_is_true(json_object_get(res, "found")), "found must be true");

    json_t* wm_obj = json_object_get(res, "watermark");
    TEST_ASSERT(wm_obj != NULL, "watermark object must exist");
    uint32_t key_id = (uint32_t)json_integer_value(json_object_get(wm_obj, "key_id"));
    TEST_ASSERT(key_id == 5566, "key_id must match 5566, got %u", key_id);
    TEST_ASSERT(json_is_true(json_object_get(wm_obj, "crc_valid")), "crc_valid must be true");

    json_decref(res);

    /* 2. 测试纯文本未检测到水印 */
    const char* clean_req = "{\"text\":\"Clean normal text without watermark\"}";
    body = NULL;
    rc = admin_dispatch(&adm,
                        "/admin/v1/watermark/decode",
                        "POST",
                        NULL,
                        "secret",
                        clean_req,
                        strlen(clean_req),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "clean request should return 200");
    json_t* clean_res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(clean_res != NULL, "valid json");
    TEST_ASSERT(json_is_false(json_object_get(clean_res, "found")), "found must be false");
    json_decref(clean_res);
}

TEST_CASE(test_admin_audit_chain_verify_endpoint)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    int rc = admin_dispatch(&adm,
                            "/admin/v1/audit/chain/verify",
                            "POST",
                            NULL,
                            "secret",
                            "{}",
                            2,
                            &status,
                            &body,
                            &len);
    TEST_ASSERT(rc == 0, "admin_dispatch should return 0, got %d", rc);
    TEST_ASSERT(status == 200, "status should be 200, got %d", status);
    TEST_ASSERT(body != NULL, "body should not be null");

    json_t* res = json_loads(body, 0, NULL);
    free(body);
    TEST_ASSERT(res != NULL, "response must be valid json");
    TEST_ASSERT(json_is_true(json_object_get(res, "valid")), "valid must be true");

    json_decref(res);
}
