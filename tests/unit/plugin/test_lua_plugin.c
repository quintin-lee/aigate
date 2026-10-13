/**
 * @file test_lua_plugin.c
 * @brief Unit tests for the Embedded Lua Script Plugin Engine and Admin Workbench.
 */

#include "plugin/lua_plugin.h"
#include "run_tests.h"
#include "server/admin_api.h"
#include "store/pg_store.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_CASE(test_lua_plugin_syntax_validation)
{
    char err[256] = {0};

    /* 1. Valid Lua code */
    const char* valid_code = "local x = 10\n"
                             "local y = 20\n"
                             "return x + y\n";
    int         rc = lua_plugin_validate_syntax(valid_code, err, sizeof(err));
    TEST_ASSERT(rc == 0, "Valid Lua code should pass syntax check");
    TEST_ASSERT(err[0] == '\0', "Error message should be empty on success");

    /* 2. Syntax error */
    const char* invalid_code = "function incomplete_fn(";
    rc = lua_plugin_validate_syntax(invalid_code, err, sizeof(err));
    TEST_ASSERT(rc != 0, "Broken Lua code must fail syntax check");
    TEST_ASSERT(strlen(err) > 0, "Error buffer should contain error details");

    /* 3. Empty code */
    rc = lua_plugin_validate_syntax("", err, sizeof(err));
    TEST_ASSERT(rc != 0, "Empty code must return error");
}

TEST_CASE(test_lua_plugin_sandbox_restrictions)
{
    /* Script trying to access unsafe globals */
    const char* script = "assert(os == nil, 'os module must be nil')\n"
                         "assert(io == nil, 'io module must be nil')\n"
                         "assert(package == nil, 'package module must be nil')\n"
                         "assert(dofile == nil, 'dofile must be nil')\n"
                         "assert(loadfile == nil, 'loadfile must be nil')\n"
                         "req:set_header('x-sandbox-safe', 'true')\n";

    char*  res_json = NULL;
    size_t res_len = 0;
    int    rc = lua_plugin_test_dry_run(script, "request", "{\"headers\":{}}", &res_json, &res_len);
    TEST_ASSERT(rc == 0 && res_json != NULL, "Dry run should execute safely");

    json_error_t jerr;
    json_t*      root = json_loads(res_json, 0, &jerr);
    TEST_ASSERT(root != NULL, "Valid JSON result");

    json_t* jsuc = json_object_get(root, "success");
    TEST_ASSERT(jsuc != NULL && json_is_true(jsuc), "Sandbox script should succeed");

    json_t* jreq = json_object_get(root, "request");
    TEST_ASSERT(jreq != NULL, "request object present");
    json_t* jheaders = json_object_get(jreq, "headers");
    TEST_ASSERT(jheaders != NULL, "headers present");
    json_t* jhdr = json_object_get(jheaders, "x-sandbox-safe");
    TEST_ASSERT(jhdr != NULL && strcmp(json_string_value(jhdr), "true") == 0,
                "x-sandbox-safe header must be set");

    json_decref(root);
    free(res_json);
}

TEST_CASE(test_lua_plugin_infinite_loop_protection)
{
    /* Infinite loop script */
    const char* script = "local count = 0\n"
                         "while true do\n"
                         "    count = count + 1\n"
                         "end\n";

    char*  res_json = NULL;
    size_t res_len = 0;
    int    rc = lua_plugin_test_dry_run(script, "request", "{}", &res_json, &res_len);
    TEST_ASSERT(rc == 0 && res_json != NULL, "Sandbox should catch infinite loop without crashing");

    json_error_t jerr;
    json_t*      root = json_loads(res_json, 0, &jerr);
    TEST_ASSERT(root != NULL, "Valid JSON result");

    json_t* jsuc = json_object_get(root, "success");
    TEST_ASSERT(jsuc != NULL && json_is_false(jsuc), "Success should be false on timeout");

    json_t* jerr_str = json_object_get(root, "error");
    TEST_ASSERT(jerr_str != NULL && json_is_string(jerr_str), "Error string must be present");
    TEST_ASSERT(strstr(json_string_value(jerr_str), "instruction quota exceeded") != NULL,
                "Must mention instruction quota exceeded");

    json_decref(root);
    free(res_json);
}

TEST_CASE(test_lua_plugin_dry_run_request_and_response)
{
    /* 1. Request phase test */
    const char* req_script = "local h = req:get_header('authorization')\n"
                             "assert(h == 'Bearer test-token')\n"
                             "req:set_header('x-gateway-trace', 'tr-123456')\n"
                             "local m = req:get_model()\n"
                             "assert(m == 'gpt-4o')\n"
                             "req:set_model('deepseek-chat')\n"
                             "local j = req:get_json()\n"
                             "assert(j and j.prompt == 'hello')\n"
                             "aigate.log_info('Processed mock request successfully')\n";

    const char* mock_req = "{\n"
                           "  \"model\": \"gpt-4o\",\n"
                           "  \"headers\": {\"authorization\": \"Bearer test-token\"},\n"
                           "  \"body\": {\"prompt\": \"hello\"}\n"
                           "}";

    char*  res_json = NULL;
    size_t res_len = 0;
    int    rc = lua_plugin_test_dry_run(req_script, "request", mock_req, &res_json, &res_len);
    TEST_ASSERT(rc == 0 && res_json != NULL, "Dry run request phase execution");

    json_error_t jerr;
    json_t*      root = json_loads(res_json, 0, &jerr);
    TEST_ASSERT(root != NULL, "Valid JSON");
    TEST_ASSERT(json_is_true(json_object_get(root, "success")), "Dry run succeeded");

    json_t* jreq = json_object_get(root, "request");
    TEST_ASSERT(jreq != NULL, "request present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jreq, "model")), "deepseek-chat") == 0,
                "Model should be rewritten to deepseek-chat");
    json_t* jhdrs = json_object_get(jreq, "headers");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jhdrs, "x-gateway-trace")), "tr-123456") ==
                    0,
                "Trace header injected");

    json_t* jlogs = json_object_get(root, "logs");
    TEST_ASSERT(jlogs != NULL && json_array_size(jlogs) > 0, "Logs present");

    json_decref(root);
    free(res_json);
    res_json = NULL;

    /* 2. Response phase test */
    const char* resp_script = "local s = resp:get_status()\n"
                              "assert(s == 200)\n"
                              "resp:set_status(206)\n"
                              "local b = resp:get_body()\n"
                              "local masked = string.gsub(b, '13812345678', '138****5678')\n"
                              "resp:set_body(masked)\n"
                              "resp:set_header('x-redacted', 'true')\n";

    const char* mock_resp = "{\n"
                            "  \"status\": 200,\n"
                            "  \"headers\": {},\n"
                            "  \"body\": \"Contact phone: 13812345678\"\n"
                            "}";

    rc = lua_plugin_test_dry_run(resp_script, "response", mock_resp, &res_json, &res_len);
    TEST_ASSERT(rc == 0 && res_json != NULL, "Dry run response phase execution");

    root = json_loads(res_json, 0, &jerr);
    TEST_ASSERT(root != NULL, "Valid JSON");
    TEST_ASSERT(json_is_true(json_object_get(root, "success")), "Response dry run succeeded");

    json_t* jresp = json_object_get(root, "response");
    TEST_ASSERT(jresp != NULL, "response present");
    TEST_ASSERT(json_integer_value(json_object_get(jresp, "status")) == 206, "Status set to 206");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jresp, "body")),
                       "Contact phone: 138****5678") == 0,
                "Body successfully masked");
    json_t* jrhdrs = json_object_get(jresp, "headers");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(jrhdrs, "x-redacted")), "true") == 0,
                "x-redacted header set");

    json_decref(root);
    free(res_json);
    res_json = NULL;

    /* 3. aigate.abort test */
    const char* abort_script = "aigate.abort(403, 'Forbidden by policy')\n";
    rc = lua_plugin_test_dry_run(abort_script, "request", "{}", &res_json, &res_len);
    TEST_ASSERT(rc == 0 && res_json != NULL, "Dry run abort executed");

    root = json_loads(res_json, 0, &jerr);
    TEST_ASSERT(root != NULL, "Valid JSON");
    TEST_ASSERT(json_is_true(json_object_get(root, "aborted")), "aborted flag is true");
    TEST_ASSERT(json_integer_value(json_object_get(root, "abort_status")) == 403, "Status is 403");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(root, "abort_reason")),
                       "Forbidden by policy") == 0,
                "Abort reason match");

    json_decref(root);
    free(res_json);
}

TEST_CASE(test_lua_plugin_mgr_lifecycle)
{
    lua_plugin_mgr_t* mgr = lua_plugin_mgr_create();
    TEST_ASSERT(mgr != NULL, "Plugin manager created");

    lua_plugin_def_t defs[2];
    memset(defs, 0, sizeof(defs));

    defs[0].id = 1;
    snprintf(defs[0].name, sizeof(defs[0].name), "header_tagger");
    snprintf(defs[0].phase, sizeof(defs[0].phase), "request");
    defs[0].priority = 10;
    defs[0].enabled = 1;
    snprintf(defs[0].match_model, sizeof(defs[0].match_model), "*");
    defs[0].script_code = strdup("req:set_header('x-p1', 'done')\n");

    defs[1].id = 2;
    snprintf(defs[1].name, sizeof(defs[1].name), "model_router");
    snprintf(defs[1].phase, sizeof(defs[1].phase), "request");
    defs[1].priority = 20;
    defs[1].enabled = 1;
    snprintf(defs[1].match_model, sizeof(defs[1].match_model), "gpt-4o");
    defs[1].script_code = strdup("req:set_model('claude-3-5-sonnet')\n");

    int rc = lua_plugin_mgr_set_plugins(mgr, defs, 2);
    TEST_ASSERT(rc == 0, "Set plugins succeeded");
    lua_plugin_def_free(&defs[0]);
    lua_plugin_def_free(&defs[1]);

    /* Execute request filter on matching model gpt-4o */
    lua_req_ctx_t req_ctx;
    memset(&req_ctx, 0, sizeof(req_ctx));
    req_ctx.model = "gpt-4o";
    snprintf(req_ctx.in_headers[0].key, sizeof(req_ctx.in_headers[0].key), "Host");
    snprintf(req_ctx.in_headers[0].value, sizeof(req_ctx.in_headers[0].value), "localhost");
    req_ctx.in_header_count = 1;
    req_ctx.raw_body = "{\"messages\":[]}";
    req_ctx.raw_body_len = strlen(req_ctx.raw_body);

    lua_exec_result_t exec_res = lua_plugin_mgr_exec_request(mgr, &req_ctx);
    TEST_ASSERT(exec_res == LUA_EXEC_OK, "Execute request filter");
    TEST_ASSERT(req_ctx.model_rewritten == true, "Model was rewritten");
    TEST_ASSERT(strcmp(req_ctx.new_model, "claude-3-5-sonnet") == 0, "Model rewritten by P2");
    TEST_ASSERT(req_ctx.out_header_count == 1, "Header count is 1");
    TEST_ASSERT(strcmp(req_ctx.out_headers[0].key, "x-p1") == 0, "Header key is x-p1");
    TEST_ASSERT(strcmp(req_ctx.out_headers[0].value, "done") == 0, "Header value is done");

    if (req_ctx.new_body != NULL) {
        free(req_ctx.new_body);
    }
    if (req_ctx.abort_body != NULL) {
        free(req_ctx.abort_body);
    }

    /* Test response phase */
    defs[0].id = 3;
    snprintf(defs[0].name, sizeof(defs[0].name), "resp_redact");
    snprintf(defs[0].phase, sizeof(defs[0].phase), "response");
    defs[0].priority = 10;
    defs[0].enabled = 1;
    snprintf(defs[0].match_model, sizeof(defs[0].match_model), "*");
    defs[0].script_code = strdup("resp:set_status(202)\nresp:set_body('rewritten')\n");

    rc = lua_plugin_mgr_set_plugins(mgr, defs, 1);
    TEST_ASSERT(rc == 0, "Set response plugin");
    lua_plugin_def_free(&defs[0]);

    lua_resp_ctx_t resp_ctx;
    memset(&resp_ctx, 0, sizeof(resp_ctx));
    resp_ctx.status = 200;
    resp_ctx.raw_body = "original";
    resp_ctx.raw_body_len = strlen("original");

    exec_res = lua_plugin_mgr_exec_response(mgr, "claude-3-5-sonnet", &resp_ctx);
    TEST_ASSERT(exec_res == LUA_EXEC_OK, "Execute response filter");
    TEST_ASSERT(resp_ctx.new_status == 202, "Status changed to 202");
    TEST_ASSERT(resp_ctx.new_body != NULL && strcmp(resp_ctx.new_body, "rewritten") == 0,
                "Body rewritten");

    if (resp_ctx.new_body != NULL) {
        free(resp_ctx.new_body);
    }
    if (resp_ctx.abort_body != NULL) {
        free(resp_ctx.abort_body);
    }

    lua_plugin_mgr_destroy(mgr);
}

/* ------------------------------------------------ fake pg store for admin tests */

#define FAKE_PLUGIN_CAP 16

typedef struct fake_plugin_store {
    lua_plugin_def_t plugins[FAKE_PLUGIN_CAP];
    int              in_use[FAKE_PLUGIN_CAP];
    int64_t          next_id;
} fake_plugin_store_t;

static int
fake_list_script_plugins(void* ctx, lua_plugin_def_t** out_recs, size_t* out_count)
{
    fake_plugin_store_t* st = (fake_plugin_store_t*)ctx;
    size_t               count = 0;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (st->in_use[i]) {
            count++;
        }
    }
    if (count == 0) {
        *out_recs = NULL;
        *out_count = 0;
        return 0;
    }
    lua_plugin_def_t* arr = calloc(count, sizeof(lua_plugin_def_t));
    size_t            idx = 0;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (st->in_use[i]) {
            memcpy(&arr[idx], &st->plugins[i], sizeof(lua_plugin_def_t));
            if (st->plugins[i].script_code != NULL) {
                arr[idx].script_code = strdup(st->plugins[i].script_code);
            }
            idx++;
        }
    }
    *out_recs = arr;
    *out_count = count;
    return 0;
}

static int
fake_get_script_plugin(void* ctx, int64_t id, lua_plugin_def_t* out_rec)
{
    fake_plugin_store_t* st = (fake_plugin_store_t*)ctx;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (st->in_use[i] && st->plugins[i].id == id) {
            memcpy(out_rec, &st->plugins[i], sizeof(lua_plugin_def_t));
            if (st->plugins[i].script_code != NULL) {
                out_rec->script_code = strdup(st->plugins[i].script_code);
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_create_script_plugin(void* ctx, const lua_plugin_def_t* rec, int64_t* out_id)
{
    fake_plugin_store_t* st = (fake_plugin_store_t*)ctx;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (!st->in_use[i]) {
            st->in_use[i] = 1;
            memcpy(&st->plugins[i], rec, sizeof(lua_plugin_def_t));
            st->plugins[i].id = ++st->next_id;
            if (rec->script_code != NULL) {
                st->plugins[i].script_code = strdup(rec->script_code);
            }
            *out_id = st->plugins[i].id;
            return 0;
        }
    }
    return -1;
}

static int
fake_update_script_plugin(void* ctx, const lua_plugin_def_t* rec)
{
    fake_plugin_store_t* st = (fake_plugin_store_t*)ctx;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (st->in_use[i] && st->plugins[i].id == rec->id) {
            if (st->plugins[i].script_code != NULL) {
                free(st->plugins[i].script_code);
            }
            memcpy(&st->plugins[i], rec, sizeof(lua_plugin_def_t));
            if (rec->script_code != NULL) {
                st->plugins[i].script_code = strdup(rec->script_code);
            }
            return 0;
        }
    }
    return -1;
}

static int
fake_delete_script_plugin(void* ctx, int64_t id)
{
    fake_plugin_store_t* st = (fake_plugin_store_t*)ctx;
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (st->in_use[i] && st->plugins[i].id == id) {
            st->in_use[i] = 0;
            if (st->plugins[i].script_code != NULL) {
                free(st->plugins[i].script_code);
                st->plugins[i].script_code = NULL;
            }
            return 0;
        }
    }
    return -1;
}

TEST_CASE(test_lua_plugin_admin_api_crud)
{
    fake_plugin_store_t fake_st;
    memset(&fake_st, 0, sizeof(fake_st));
    fake_st.next_id = 1000;

    pg_ops_t fake_ops;
    memset(&fake_ops, 0, sizeof(fake_ops));
    fake_ops.ctx = &fake_st;
    fake_ops.list_script_plugins = fake_list_script_plugins;
    fake_ops.get_script_plugin = fake_get_script_plugin;
    fake_ops.create_script_plugin = fake_create_script_plugin;
    fake_ops.update_script_plugin = fake_update_script_plugin;
    fake_ops.delete_script_plugin = fake_delete_script_plugin;

    pg_store_t* ps = pg_store_open(NULL, &fake_ops);
    TEST_ASSERT(ps != NULL, "pg_store_open with fake_ops succeeded");

    aigate_core ac;
    memset(&ac, 0, sizeof(ac));
    ac.plugins = lua_plugin_mgr_create();

    admin_ctx_t adm;
    memset(&adm, 0, sizeof(adm));
    adm.ps = ps;
    adm.ac = &ac;
    /* SHA-256 of "secret" */
    adm.admin_token_hash = "2bb80d537b1da3e38bd30361aa855686bde0eacd7162fef6a25fe97bf527a25b";

    int    status = 0;
    char*  body = NULL;
    size_t len = 0;

    /* 1. GET /admin/v1/plugins -> empty array */
    int rc = admin_dispatch(
        &adm, "/admin/v1/plugins", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET /admin/v1/plugins succeeds");
    json_t* res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "Valid JSON");
    json_t* arr = json_object_get(res, "plugins");
    TEST_ASSERT(arr != NULL && json_array_size(arr) == 0, "plugins array is empty");
    json_decref(res);

    /* 2. POST /admin/v1/plugins with syntax error -> 400 */
    const char* bad_payload =
        "{\"name\":\"bad\",\"phase\":\"request\",\"script_code\":\"function broken(\"}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/plugins",
                        "POST",
                        NULL,
                        "secret",
                        bad_payload,
                        strlen(bad_payload),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 400, "Broken Lua syntax returns 400");
    free(body);
    body = NULL;

    /* 3. POST /admin/v1/plugins with valid plugin -> 201 Created */
    const char* good_payload = "{\"name\":\"tagger\",\"phase\":\"request\",\"priority\":5,\"match_"
                               "model\":\"*\",\"enabled\":true,\"description\":\"test plugin\","
                               "\"script_code\":\"req:set_header('x-tag', '1')\"}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/plugins",
                        "POST",
                        NULL,
                        "secret",
                        good_payload,
                        strlen(good_payload),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 201, "Create plugin returns 201");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "Valid JSON");
    int64_t created_id = (int64_t)json_integer_value(json_object_get(res, "id"));
    TEST_ASSERT(created_id == 1001, "Created ID is 1001");
    json_decref(res);

    /* 4. GET /admin/v1/plugins/1001 */
    rc = admin_dispatch(
        &adm, "/admin/v1/plugins/1001", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Get plugin by ID returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL, "Valid JSON");
    json_t* pobj = json_object_get(res, "plugin");
    TEST_ASSERT(pobj != NULL, "plugin object present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(pobj, "name")), "tagger") == 0,
                "Name matches");
    TEST_ASSERT(json_integer_value(json_object_get(pobj, "priority")) == 5, "Priority matches");
    json_decref(res);

    /* 5. PUT /admin/v1/plugins/1001 -> update priority to 15 */
    const char* put_payload = "{\"priority\":15,\"enabled\":false}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/plugins/1001",
                        "PUT",
                        NULL,
                        "secret",
                        put_payload,
                        strlen(put_payload),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "Update plugin returns 200");
    free(body);
    body = NULL;

    /* Verify updated record */
    rc = admin_dispatch(
        &adm, "/admin/v1/plugins/1001", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Get plugin again");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    pobj = json_object_get(res, "plugin");
    TEST_ASSERT(json_integer_value(json_object_get(pobj, "priority")) == 15, "Priority is now 15");
    TEST_ASSERT(json_is_false(json_object_get(pobj, "enabled")), "Enabled is now false");
    json_decref(res);

    /* 6. POST /admin/v1/plugins/test -> dry-run sandbox */
    const char* test_payload = "{\"phase\":\"request\",\"script_code\":\"req:set_header('x-test', "
                               "'ok')\",\"mock_payload\":{\"headers\":{}}}";
    rc = admin_dispatch(&adm,
                        "/admin/v1/plugins/test",
                        "POST",
                        NULL,
                        "secret",
                        test_payload,
                        strlen(test_payload),
                        &status,
                        &body,
                        &len);
    TEST_ASSERT(rc == 0 && status == 200, "Dry-run test via API returns 200");
    res = json_loads(body, 0, NULL);
    free(body);
    body = NULL;
    TEST_ASSERT(res != NULL && json_is_true(json_object_get(res, "success")),
                "Dry run sandbox succeeded");
    json_decref(res);

    /* 7. DELETE /admin/v1/plugins/1001 */
    rc = admin_dispatch(
        &adm, "/admin/v1/plugins/1001", "DELETE", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "Delete plugin returns 200");
    free(body);
    body = NULL;

    /* 8. GET /admin/v1/plugins/1001 -> 404 */
    rc = admin_dispatch(
        &adm, "/admin/v1/plugins/1001", "GET", NULL, "secret", NULL, 0, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 404, "Deleted plugin returns 404");
    free(body);
    body = NULL;

    lua_plugin_mgr_destroy(ac.plugins);
    pg_store_close(ps);
    for (int i = 0; i < FAKE_PLUGIN_CAP; i++) {
        if (fake_st.in_use[i] && fake_st.plugins[i].script_code != NULL) {
            free(fake_st.plugins[i].script_code);
        }
    }
}
