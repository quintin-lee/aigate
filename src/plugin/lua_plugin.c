/**
 * @file lua_plugin.c
 * @brief Implementation of the embedded Lua script plugin engine for aigate.
 */

#include "lua_plugin.h"
#include "aigate_log.h"
#include "sha256.h"

#include <ctype.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

/* Special Lua registry key identifying the current C execution context */
#define REG_REQ_CTX_KEY "__aigate_req_ctx"
#define REG_RESP_CTX_KEY "__aigate_resp_ctx"
#define REG_LOG_ARR_KEY "__aigate_test_logs"

struct lua_plugin_mgr {
    lua_plugin_def_t* plugins;
    size_t            count;
    pthread_rwlock_t  lock;
};

/* -------------------------------------------------------------------------
 * Internal JSON <-> Lua Conversion Helpers (via Jansson)
 * ------------------------------------------------------------------------- */

static void
json_to_lua(lua_State* L, json_t* val)
{
    if (val == NULL || json_is_null(val)) {
        lua_pushnil(L);
    } else if (json_is_boolean(val)) {
        lua_pushboolean(L, json_is_true(val) ? 1 : 0);
    } else if (json_is_integer(val)) {
        lua_pushinteger(L, (lua_Integer)json_integer_value(val));
    } else if (json_is_real(val)) {
        lua_pushnumber(L, (lua_Number)json_real_value(val));
    } else if (json_is_string(val)) {
        lua_pushlstring(L, json_string_value(val), json_string_length(val));
    } else if (json_is_array(val)) {
        size_t n = json_array_size(val);
        lua_createtable(L, (int)n, 0);
        for (size_t i = 0; i < n; i++) {
            json_to_lua(L, json_array_get(val, i));
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
    } else if (json_is_object(val)) {
        size_t      n = json_object_size(val);
        const char* key;
        json_t*     child;
        lua_createtable(L, 0, (int)n);
        json_object_foreach(val, key, child)
        {
            json_to_lua(L, child);
            lua_setfield(L, -2, key);
        }
    } else {
        lua_pushnil(L);
    }
}

static json_t*
lua_to_json(lua_State* L, int idx)
{
    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:
        return json_null();
    case LUA_TBOOLEAN:
        return lua_toboolean(L, idx) ? json_true() : json_false();
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            return json_integer(lua_tointeger(L, idx));
        } else {
            return json_real(lua_tonumber(L, idx));
        }
    case LUA_TSTRING:
        return json_string(lua_tostring(L, idx));
    case LUA_TTABLE: {
        /* Check if array (consecutive integer keys 1..N) */
        lua_len(L, idx);
        lua_Integer len = lua_tointeger(L, -1);
        lua_pop(L, 1);

        if (len > 0) {
            json_t* arr = json_array();
            for (lua_Integer i = 1; i <= len; i++) {
                lua_rawgeti(L, idx, i);
                json_array_append_new(arr, lua_to_json(L, -1));
                lua_pop(L, 1);
            }
            return arr;
        }

        /* Object mapping */
        json_t* obj = json_object();
        lua_pushvalue(L, idx);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char* k = lua_tostring(L, -2);
                json_object_set_new(obj, k, lua_to_json(L, -1));
            } else if (lua_isinteger(L, -2)) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "%lld", (long long)lua_tointeger(L, -2));
                json_object_set_new(obj, kbuf, lua_to_json(L, -1));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return obj;
    }
    default:
        return json_null();
    }
}

/* -------------------------------------------------------------------------
 * Instruction Limiter Hook (Anti-Infinite-Loop Sandbox Protection)
 * ------------------------------------------------------------------------- */

static void
lua_instruction_hook(lua_State* L, lua_Debug* ar)
{
    (void)ar;
    luaL_error(L, "execution instruction quota exceeded (possible infinite loop)");
}

/* -------------------------------------------------------------------------
 * Wildcard Matching Helper
 * ------------------------------------------------------------------------- */

static bool
wildcard_match(const char* pattern, const char* str)
{
    if (pattern == NULL || pattern[0] == '\0' || strcmp(pattern, "*") == 0) {
        return true;
    }
    if (str == NULL) {
        return false;
    }

    const char* p = pattern;
    const char* s = str;
    const char* star_p = NULL;
    const char* match_s = NULL;

    while (*s != '\0') {
        if (*p == *s) {
            p++;
            s++;
        } else if (*p == '*') {
            star_p = p++;
            match_s = s;
        } else if (star_p != NULL) {
            p = star_p + 1;
            match_s++;
            s = match_s;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        p++;
    }
    return (*p == '\0');
}

/* -------------------------------------------------------------------------
 * Global aigate Module Native Bindings
 * ------------------------------------------------------------------------- */

static void
append_test_log(lua_State* L, const char* level, const char* msg)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_LOG_ARR_KEY);
    if (lua_istable(L, -1)) {
        char line[1024];
        snprintf(line, sizeof(line), "[%s] %s", level, msg);
        lua_len(L, -1);
        lua_Integer len = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushstring(L, line);
        lua_rawseti(L, -2, len + 1);
    }
    lua_pop(L, 1);
}

static int
l_aigate_log_info(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    AIGATE_LOG_INFO("[plugin] %s", msg);
    append_test_log(L, "INFO", msg);
    return 0;
}

static int
l_aigate_log_warn(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    AIGATE_LOG_WARN("[plugin] %s", msg);
    append_test_log(L, "WARN", msg);
    return 0;
}

static int
l_aigate_log_error(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    AIGATE_LOG_ERROR("[plugin] %s", msg);
    append_test_log(L, "ERROR", msg);
    return 0;
}

static int
l_aigate_sha256(lua_State* L)
{
    size_t      len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    char        hex[65] = {0};
    sha256_hex((const uint8_t*)str, len, hex);
    lua_pushstring(L, hex);
    return 1;
}

static int
l_aigate_json_decode(lua_State* L)
{
    size_t      len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    if (len == 0) {
        lua_pushnil(L);
        return 1;
    }
    json_error_t err;
    json_t*      root = json_loadb(str, len, 0, &err);
    if (!root) {
        lua_pushnil(L);
        lua_pushstring(L, err.text);
        return 2;
    }
    json_to_lua(L, root);
    json_decref(root);
    return 1;
}

static int
l_aigate_json_encode(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    json_t* root = lua_to_json(L, 1);
    if (!root) {
        lua_pushnil(L);
        return 1;
    }
    char* encoded = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!encoded) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, encoded);
    free(encoded);
    return 1;
}

static int
l_aigate_abort(lua_State* L)
{
    int         status = (int)luaL_checkinteger(L, 1);
    const char* body = "";
    char*       freeme = NULL;

    if (lua_istable(L, 2)) {
        json_t* root = lua_to_json(L, 2);
        if (root) {
            freeme = json_dumps(root, JSON_COMPACT);
            json_decref(root);
            if (freeme) {
                body = freeme;
            }
        }
    } else if (lua_isstring(L, 2)) {
        body = lua_tostring(L, 2);
    }

    /* Check if in request or response phase */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* req_ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (req_ctx != NULL) {
        req_ctx->abort_status = status;
        if (req_ctx->abort_body != NULL) {
            free(req_ctx->abort_body);
        }
        req_ctx->abort_body = strdup(body);
        req_ctx->abort_body_len = strlen(body);
    } else {
        lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
        lua_resp_ctx_t* resp_ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        if (resp_ctx != NULL) {
            resp_ctx->abort_status = status;
            if (resp_ctx->abort_body != NULL) {
                free(resp_ctx->abort_body);
            }
            resp_ctx->abort_body = strdup(body);
            resp_ctx->abort_body_len = strlen(body);
        }
    }

    if (freeme != NULL) {
        free(freeme);
    }

    return luaL_error(L, "__aigate_abort__");
}

/* -------------------------------------------------------------------------
 * Request (req) Object Metatable and Methods
 * ------------------------------------------------------------------------- */

static int
l_req_get_header(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char* key = luaL_checkstring(L, 2);
    if (ctx == NULL) {
        lua_pushnil(L);
        return 1;
    }

    /* Check modified headers first */
    for (size_t i = 0; i < ctx->out_header_count; i++) {
        if (strcasecmp(ctx->out_headers[i].key, key) == 0) {
            lua_pushstring(L, ctx->out_headers[i].value);
            return 1;
        }
    }
    /* Check input headers */
    for (size_t i = 0; i < ctx->in_header_count; i++) {
        if (strcasecmp(ctx->in_headers[i].key, key) == 0) {
            lua_pushstring(L, ctx->in_headers[i].value);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

static int
l_req_set_header(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char* key = luaL_checkstring(L, 2);
    const char* val = luaL_checkstring(L, 3);
    if (ctx == NULL) {
        return 0;
    }

    /* Overwrite if already present in out_headers */
    for (size_t i = 0; i < ctx->out_header_count; i++) {
        if (strcasecmp(ctx->out_headers[i].key, key) == 0) {
            snprintf(ctx->out_headers[i].value, sizeof(ctx->out_headers[i].value), "%s", val);
            return 0;
        }
    }

    if (ctx->out_header_count < LUA_PLUGIN_MAX_HEADERS) {
        snprintf(ctx->out_headers[ctx->out_header_count].key,
                 sizeof(ctx->out_headers[ctx->out_header_count].key),
                 "%s",
                 key);
        snprintf(ctx->out_headers[ctx->out_header_count].value,
                 sizeof(ctx->out_headers[ctx->out_header_count].value),
                 "%s",
                 val);
        ctx->out_header_count++;
    }
    return 0;
}

static int
l_req_get_body(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx == NULL) {
        lua_pushstring(L, "");
        return 1;
    }
    if (ctx->new_body != NULL) {
        lua_pushlstring(L, ctx->new_body, ctx->new_body_len);
    } else if (ctx->raw_body != NULL) {
        lua_pushlstring(L, ctx->raw_body, ctx->raw_body_len);
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

static int
l_req_get_json(lua_State* L)
{
    l_req_get_body(L);
    size_t      len = 0;
    const char* body = lua_tolstring(L, -1, &len);
    if (len == 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    json_error_t err;
    json_t*      root = json_loadb(body, len, 0, &err);
    lua_pop(L, 1);
    if (!root) {
        lua_pushnil(L);
        return 1;
    }
    json_to_lua(L, root);
    json_decref(root);
    return 1;
}

static int
l_req_set_body(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx == NULL) {
        return 0;
    }

    if (lua_istable(L, 2)) {
        json_t* root = lua_to_json(L, 2);
        if (root) {
            char* enc = json_dumps(root, JSON_COMPACT);
            json_decref(root);
            if (enc) {
                if (ctx->new_body != NULL) {
                    free(ctx->new_body);
                }
                ctx->new_body = enc;
                ctx->new_body_len = strlen(enc);
            }
        }
    } else if (lua_isstring(L, 2)) {
        size_t      len = 0;
        const char* str = lua_tolstring(L, 2, &len);
        if (ctx->new_body != NULL) {
            free(ctx->new_body);
        }
        ctx->new_body = malloc(len + 1);
        if (ctx->new_body) {
            memcpy(ctx->new_body, str, len);
            ctx->new_body[len] = '\0';
            ctx->new_body_len = len;
        }
    }
    return 0;
}

static int
l_req_get_model(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx != NULL) {
        if (ctx->model_rewritten && ctx->new_model[0] != '\0') {
            lua_pushstring(L, ctx->new_model);
        } else if (ctx->model != NULL) {
            lua_pushstring(L, ctx->model);
        } else {
            lua_pushstring(L, "");
        }
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

static int
l_req_set_model(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);
    lua_req_ctx_t* ctx = (lua_req_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char* m = luaL_checkstring(L, 2);
    if (ctx != NULL) {
        snprintf(ctx->new_model, sizeof(ctx->new_model), "%s", m);
        ctx->model_rewritten = true;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Response (resp) Object Metatable and Methods
 * ------------------------------------------------------------------------- */

static int
l_resp_get_header(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char* key = luaL_checkstring(L, 2);
    if (ctx == NULL) {
        lua_pushnil(L);
        return 1;
    }

    for (size_t i = 0; i < ctx->out_header_count; i++) {
        if (strcasecmp(ctx->out_headers[i].key, key) == 0) {
            lua_pushstring(L, ctx->out_headers[i].value);
            return 1;
        }
    }
    for (size_t i = 0; i < ctx->in_header_count; i++) {
        if (strcasecmp(ctx->in_headers[i].key, key) == 0) {
            lua_pushstring(L, ctx->in_headers[i].value);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

static int
l_resp_set_header(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char* key = luaL_checkstring(L, 2);
    const char* val = luaL_checkstring(L, 3);
    if (ctx == NULL) {
        return 0;
    }

    for (size_t i = 0; i < ctx->out_header_count; i++) {
        if (strcasecmp(ctx->out_headers[i].key, key) == 0) {
            snprintf(ctx->out_headers[i].value, sizeof(ctx->out_headers[i].value), "%s", val);
            return 0;
        }
    }

    if (ctx->out_header_count < LUA_PLUGIN_MAX_HEADERS) {
        snprintf(ctx->out_headers[ctx->out_header_count].key,
                 sizeof(ctx->out_headers[ctx->out_header_count].key),
                 "%s",
                 key);
        snprintf(ctx->out_headers[ctx->out_header_count].value,
                 sizeof(ctx->out_headers[ctx->out_header_count].value),
                 "%s",
                 val);
        ctx->out_header_count++;
    }
    return 0;
}

static int
l_resp_get_body(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx == NULL) {
        lua_pushstring(L, "");
        return 1;
    }
    if (ctx->new_body != NULL) {
        lua_pushlstring(L, ctx->new_body, ctx->new_body_len);
    } else if (ctx->raw_body != NULL) {
        lua_pushlstring(L, ctx->raw_body, ctx->raw_body_len);
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

static int
l_resp_get_json(lua_State* L)
{
    l_resp_get_body(L);
    size_t      len = 0;
    const char* body = lua_tolstring(L, -1, &len);
    if (len == 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    json_error_t err;
    json_t*      root = json_loadb(body, len, 0, &err);
    lua_pop(L, 1);
    if (!root) {
        lua_pushnil(L);
        return 1;
    }
    json_to_lua(L, root);
    json_decref(root);
    return 1;
}

static int
l_resp_set_body(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx == NULL) {
        return 0;
    }

    if (lua_istable(L, 2)) {
        json_t* root = lua_to_json(L, 2);
        if (root) {
            char* enc = json_dumps(root, JSON_COMPACT);
            json_decref(root);
            if (enc) {
                if (ctx->new_body != NULL) {
                    free(ctx->new_body);
                }
                ctx->new_body = enc;
                ctx->new_body_len = strlen(enc);
            }
        }
    } else if (lua_isstring(L, 2)) {
        size_t      len = 0;
        const char* str = lua_tolstring(L, 2, &len);
        if (ctx->new_body != NULL) {
            free(ctx->new_body);
        }
        ctx->new_body = malloc(len + 1);
        if (ctx->new_body) {
            memcpy(ctx->new_body, str, len);
            ctx->new_body[len] = '\0';
            ctx->new_body_len = len;
        }
    }
    return 0;
}

static int
l_resp_get_status(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (ctx != NULL) {
        int st = ctx->new_status != 0 ? ctx->new_status : ctx->status;
        lua_pushinteger(L, st);
    } else {
        lua_pushinteger(L, 200);
    }
    return 1;
}

static int
l_resp_set_status(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);
    lua_resp_ctx_t* ctx = (lua_resp_ctx_t*)lua_touserdata(L, -1);
    lua_pop(L, 1);

    int st = (int)luaL_checkinteger(L, 2);
    if (ctx != NULL) {
        ctx->new_status = st;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Sandbox Initialization & Environment Creation
 * ------------------------------------------------------------------------- */

static lua_State*
lua_create_sandbox(void)
{
    lua_State* L = luaL_newstate();
    if (!L) {
        return NULL;
    }

    /* Open safe standard libraries */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    /* Strip unsafe base functions */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "collectgarbage");

    /* Ensure os, io, package are nil */
    lua_pushnil(L);
    lua_setglobal(L, "os");
    lua_pushnil(L);
    lua_setglobal(L, "io");
    lua_pushnil(L);
    lua_setglobal(L, "package");
    lua_pushnil(L);
    lua_setglobal(L, "coroutine");

    /* Install instruction limiter */
    lua_sethook(L, lua_instruction_hook, LUA_MASKCOUNT, LUA_PLUGIN_MAX_INSTRUCTIONS);

    /* Register global 'aigate' table */
    lua_newtable(L);

    lua_pushcfunction(L, l_aigate_log_info);
    lua_setfield(L, -2, "log_info");
    lua_pushcfunction(L, l_aigate_log_warn);
    lua_setfield(L, -2, "log_warn");
    lua_pushcfunction(L, l_aigate_log_error);
    lua_setfield(L, -2, "log_error");

    lua_pushcfunction(L, l_aigate_sha256);
    lua_setfield(L, -2, "sha256");
    lua_pushcfunction(L, l_aigate_json_decode);
    lua_setfield(L, -2, "json_decode");
    lua_pushcfunction(L, l_aigate_json_encode);
    lua_setfield(L, -2, "json_encode");
    lua_pushcfunction(L, l_aigate_abort);
    lua_setfield(L, -2, "abort");

    lua_setglobal(L, "aigate");

    return L;
}

static void
lua_bind_req(lua_State* L, lua_req_ctx_t* ctx)
{
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_REQ_CTX_KEY);

    /* Create 'req' table */
    lua_newtable(L);

    lua_pushstring(L, ctx->method ? ctx->method : "POST");
    lua_setfield(L, -2, "method");
    lua_pushstring(L, ctx->path ? ctx->path : "");
    lua_setfield(L, -2, "path");
    lua_pushstring(L, ctx->client_ip ? ctx->client_ip : "127.0.0.1");
    lua_setfield(L, -2, "client_ip");
    lua_pushstring(L, ctx->model ? ctx->model : "");
    lua_setfield(L, -2, "model");

    /* Populate req.headers table */
    lua_newtable(L);
    for (size_t i = 0; i < ctx->in_header_count; i++) {
        lua_pushstring(L, ctx->in_headers[i].value);
        lua_setfield(L, -2, ctx->in_headers[i].key);
    }
    lua_setfield(L, -2, "headers");

    /* Attach methods */
    lua_pushcfunction(L, l_req_get_header);
    lua_setfield(L, -2, "get_header");
    lua_pushcfunction(L, l_req_set_header);
    lua_setfield(L, -2, "set_header");
    lua_pushcfunction(L, l_req_get_body);
    lua_setfield(L, -2, "get_body");
    lua_pushcfunction(L, l_req_get_json);
    lua_setfield(L, -2, "get_json");
    lua_pushcfunction(L, l_req_set_body);
    lua_setfield(L, -2, "set_body");
    lua_pushcfunction(L, l_req_get_model);
    lua_setfield(L, -2, "get_model");
    lua_pushcfunction(L, l_req_set_model);
    lua_setfield(L, -2, "set_model");

    lua_setglobal(L, "req");
}

static void
lua_bind_resp(lua_State* L, lua_resp_ctx_t* ctx)
{
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_RESP_CTX_KEY);

    /* Create 'resp' table */
    lua_newtable(L);

    lua_pushinteger(L, ctx->new_status != 0 ? ctx->new_status : ctx->status);
    lua_setfield(L, -2, "status");

    lua_newtable(L);
    for (size_t i = 0; i < ctx->in_header_count; i++) {
        lua_pushstring(L, ctx->in_headers[i].value);
        lua_setfield(L, -2, ctx->in_headers[i].key);
    }
    lua_setfield(L, -2, "headers");

    lua_pushcfunction(L, l_resp_get_header);
    lua_setfield(L, -2, "get_header");
    lua_pushcfunction(L, l_resp_set_header);
    lua_setfield(L, -2, "set_header");
    lua_pushcfunction(L, l_resp_get_body);
    lua_setfield(L, -2, "get_body");
    lua_pushcfunction(L, l_resp_get_json);
    lua_setfield(L, -2, "get_json");
    lua_pushcfunction(L, l_resp_set_body);
    lua_setfield(L, -2, "set_body");
    lua_pushcfunction(L, l_resp_get_status);
    lua_setfield(L, -2, "get_status");
    lua_pushcfunction(L, l_resp_set_status);
    lua_setfield(L, -2, "set_status");

    lua_setglobal(L, "resp");
}

/* -------------------------------------------------------------------------
 * Execution & Lifecycle Implementation
 * ------------------------------------------------------------------------- */

lua_plugin_mgr_t*
lua_plugin_mgr_create(void)
{
    lua_plugin_mgr_t* mgr = calloc(1, sizeof(lua_plugin_mgr_t));
    if (!mgr) {
        return NULL;
    }
    pthread_rwlock_init(&mgr->lock, NULL);
    return mgr;
}

void
lua_plugin_mgr_destroy(lua_plugin_mgr_t* mgr)
{
    if (!mgr) {
        return;
    }
    pthread_rwlock_wrlock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        lua_plugin_def_free(&mgr->plugins[i]);
    }
    if (mgr->plugins) {
        free(mgr->plugins);
        mgr->plugins = NULL;
    }
    mgr->count = 0;
    pthread_rwlock_unlock(&mgr->lock);
    pthread_rwlock_destroy(&mgr->lock);
    free(mgr);
}

void
lua_plugin_def_free(lua_plugin_def_t* def)
{
    if (def && def->script_code) {
        free(def->script_code);
        def->script_code = NULL;
    }
}

int
lua_plugin_mgr_set_plugins(lua_plugin_mgr_t* mgr, const lua_plugin_def_t* defs, size_t count)
{
    if (!mgr) {
        return -1;
    }

    lua_plugin_def_t* new_plugins = NULL;
    if (count > 0 && defs != NULL) {
        new_plugins = calloc(count, sizeof(lua_plugin_def_t));
        if (!new_plugins) {
            return -1;
        }
        for (size_t i = 0; i < count; i++) {
            new_plugins[i] = defs[i];
            if (defs[i].script_code) {
                new_plugins[i].script_code = strdup(defs[i].script_code);
            }
        }
    }

    pthread_rwlock_wrlock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        lua_plugin_def_free(&mgr->plugins[i]);
    }
    if (mgr->plugins) {
        free(mgr->plugins);
    }
    mgr->plugins = new_plugins;
    mgr->count = count;
    pthread_rwlock_unlock(&mgr->lock);

    return 0;
}

int
lua_plugin_validate_syntax(const char* script_code, char* err_buf, size_t err_buf_cap)
{
    if (err_buf && err_buf_cap > 0) {
        err_buf[0] = '\0';
    }
    if (!script_code || script_code[0] == '\0') {
        if (err_buf && err_buf_cap > 0) {
            snprintf(err_buf, err_buf_cap, "script code is empty");
        }
        return -1;
    }

    lua_State* L = luaL_newstate();
    if (!L) {
        if (err_buf && err_buf_cap > 0) {
            snprintf(err_buf, err_buf_cap, "failed to initialize Lua state");
        }
        return -1;
    }

    int rc = luaL_loadstring(L, script_code);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (err_buf && err_buf_cap > 0) {
            snprintf(err_buf, err_buf_cap, "%s", msg ? msg : "syntax error");
        }
        lua_close(L);
        return -1;
    }

    lua_close(L);
    return 0;
}

static lua_exec_result_t
lua_execute_single_script(
    const char* script_code, const char* entrypoint, lua_State* L, char* err_buf, size_t err_cap)
{
    if (err_buf && err_cap > 0) {
        err_buf[0] = '\0';
    }
    if (luaL_loadstring(L, script_code) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "%s", err ? err : "compilation error");
        }
        AIGATE_LOG_ERROR("Lua script compilation error: %s", err ? err : "unknown");
        return LUA_EXEC_ERROR;
    }

    /* Execute the top-level script chunk */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        if (err && strstr(err, "__aigate_abort__") != NULL) {
            return LUA_EXEC_ABORT;
        }
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "%s", err ? err : "execution error");
        }
        AIGATE_LOG_ERROR("Lua script initialization error: %s", err ? err : "unknown");
        return LUA_EXEC_ERROR;
    }

    /* Call entrypoint hook function if defined (e.g. on_request or on_response) */
    lua_getglobal(L, entrypoint);
    if (lua_isfunction(L, -1)) {
        /* Pass req or resp as first argument */
        if (strcmp(entrypoint, "on_request") == 0) {
            lua_getglobal(L, "req");
        } else {
            lua_getglobal(L, "resp");
        }
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            if (err && strstr(err, "__aigate_abort__") != NULL) {
                return LUA_EXEC_ABORT;
            }
            if (err_buf && err_cap > 0) {
                snprintf(err_buf, err_cap, "%s", err ? err : "hook runtime error");
            }
            AIGATE_LOG_ERROR("Lua hook %s error: %s", entrypoint, err ? err : "unknown");
            return LUA_EXEC_ERROR;
        }
    } else {
        lua_pop(L, 1);
    }

    return LUA_EXEC_OK;
}

lua_exec_result_t
lua_plugin_mgr_exec_request(lua_plugin_mgr_t* mgr, lua_req_ctx_t* ctx)
{
    if (!mgr || !ctx) {
        return LUA_EXEC_OK;
    }

    pthread_rwlock_rdlock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        lua_plugin_def_t* p = &mgr->plugins[i];
        if (!p->enabled || !p->script_code) {
            continue;
        }
        if (strcmp(p->phase, "request") != 0 && strcmp(p->phase, "both") != 0) {
            continue;
        }
        const char* cur_model = ctx->model_rewritten ? ctx->new_model : ctx->model;
        if (!wildcard_match(p->match_model, cur_model)) {
            continue;
        }

        lua_State* L = lua_create_sandbox();
        if (!L) {
            continue;
        }
        lua_bind_req(L, ctx);

        lua_exec_result_t res = lua_execute_single_script(p->script_code, "on_request", L, NULL, 0);
        lua_close(L);

        if (res == LUA_EXEC_ABORT) {
            pthread_rwlock_unlock(&mgr->lock);
            return LUA_EXEC_ABORT;
        } else if (res == LUA_EXEC_ERROR) {
            /* Logged; continue to next plugin or gracefully fail */
        }
    }
    pthread_rwlock_unlock(&mgr->lock);
    return LUA_EXEC_OK;
}

lua_exec_result_t
lua_plugin_mgr_exec_response(lua_plugin_mgr_t* mgr, const char* model, lua_resp_ctx_t* ctx)
{
    if (!mgr || !ctx) {
        return LUA_EXEC_OK;
    }

    pthread_rwlock_rdlock(&mgr->lock);
    for (size_t i = 0; i < mgr->count; i++) {
        lua_plugin_def_t* p = &mgr->plugins[i];
        if (!p->enabled || !p->script_code) {
            continue;
        }
        if (strcmp(p->phase, "response") != 0 && strcmp(p->phase, "both") != 0) {
            continue;
        }
        if (!wildcard_match(p->match_model, model)) {
            continue;
        }

        lua_State* L = lua_create_sandbox();
        if (!L) {
            continue;
        }
        lua_bind_resp(L, ctx);

        lua_exec_result_t res =
            lua_execute_single_script(p->script_code, "on_response", L, NULL, 0);
        lua_close(L);

        if (res == LUA_EXEC_ABORT) {
            pthread_rwlock_unlock(&mgr->lock);
            return LUA_EXEC_ABORT;
        }
    }
    pthread_rwlock_unlock(&mgr->lock);
    return LUA_EXEC_OK;
}

void
lua_req_ctx_cleanup(lua_req_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->new_body) {
        free(ctx->new_body);
        ctx->new_body = NULL;
    }
    if (ctx->abort_body) {
        free(ctx->abort_body);
        ctx->abort_body = NULL;
    }
}

void
lua_resp_ctx_cleanup(lua_resp_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->new_body) {
        free(ctx->new_body);
        ctx->new_body = NULL;
    }
    if (ctx->abort_body) {
        free(ctx->abort_body);
        ctx->abort_body = NULL;
    }
}

int
lua_plugin_test_dry_run(const char* script_code,
                        const char* phase,
                        const char* mock_payload_json,
                        char**      out_result_json,
                        size_t*     out_result_len)
{
    if (!script_code || !out_result_json) {
        return -1;
    }

    /* Syntax check first */
    char syn_err[256] = {0};
    if (lua_plugin_validate_syntax(script_code, syn_err, sizeof(syn_err)) != 0) {
        json_t* err_obj = json_object();
        json_object_set_new(err_obj, "status", json_string("error"));
        json_object_set_new(err_obj, "error_type", json_string("syntax_error"));
        json_object_set_new(err_obj, "message", json_string(syn_err));
        *out_result_json = json_dumps(err_obj, JSON_COMPACT);
        *out_result_len = strlen(*out_result_json);
        json_decref(err_obj);
        return 0;
    }

    json_error_t jerr;
    json_t*      mock_root = json_loads(mock_payload_json ? mock_payload_json : "{}", 0, &jerr);
    if (!mock_root || !json_is_object(mock_root)) {
        if (mock_root) {
            json_decref(mock_root);
        }
        mock_root = json_object();
    }

    bool is_resp = (phase && strcmp(phase, "response") == 0);

    lua_State* L = lua_create_sandbox();
    if (!L) {
        json_decref(mock_root);
        return -1;
    }

    /* Initialize log array in registry */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_LOG_ARR_KEY);

    lua_req_ctx_t  req_ctx;
    lua_resp_ctx_t resp_ctx;
    memset(&req_ctx, 0, sizeof(req_ctx));
    memset(&resp_ctx, 0, sizeof(resp_ctx));

    /* Parse mock JSON */
    const char* method = json_string_value(json_object_get(mock_root, "method"));
    const char* path = json_string_value(json_object_get(mock_root, "path"));
    const char* client_ip = json_string_value(json_object_get(mock_root, "client_ip"));
    const char* model = json_string_value(json_object_get(mock_root, "model"));
    json_t*     jbody = json_object_get(mock_root, "body");
    char*       body_str = NULL;
    if (jbody) {
        if (json_is_string(jbody)) {
            body_str = strdup(json_string_value(jbody));
        } else {
            body_str = json_dumps(jbody, JSON_COMPACT);
        }
    } else {
        body_str = strdup("{}");
    }

    json_t* jheaders = json_object_get(mock_root, "headers");

    lua_exec_result_t res = LUA_EXEC_OK;
    char              exec_err[512] = {0};

    if (!is_resp) {
        req_ctx.method = method ? method : "POST";
        req_ctx.path = path ? path : "/v1/chat/completions";
        req_ctx.client_ip = client_ip ? client_ip : "127.0.0.1";
        req_ctx.model = model ? model : "gpt-4o";
        req_ctx.raw_body = body_str;
        req_ctx.raw_body_len = strlen(body_str);

        if (jheaders && json_is_object(jheaders)) {
            const char* hk;
            json_t*     hv;
            json_object_foreach(jheaders, hk, hv)
            {
                if (req_ctx.in_header_count < LUA_PLUGIN_MAX_HEADERS && json_is_string(hv)) {
                    snprintf(req_ctx.in_headers[req_ctx.in_header_count].key,
                             sizeof(req_ctx.in_headers[0].key),
                             "%s",
                             hk);
                    snprintf(req_ctx.in_headers[req_ctx.in_header_count].value,
                             sizeof(req_ctx.in_headers[0].value),
                             "%s",
                             json_string_value(hv));
                    req_ctx.in_header_count++;
                }
            }
        }

        lua_bind_req(L, &req_ctx);
        res = lua_execute_single_script(script_code, "on_request", L, exec_err, sizeof(exec_err));
    } else {
        json_t* jstatus = json_object_get(mock_root, "status");
        resp_ctx.status =
            jstatus && json_is_integer(jstatus) ? (int)json_integer_value(jstatus) : 200;
        resp_ctx.raw_body = body_str;
        resp_ctx.raw_body_len = strlen(body_str);

        if (jheaders && json_is_object(jheaders)) {
            const char* hk;
            json_t*     hv;
            json_object_foreach(jheaders, hk, hv)
            {
                if (resp_ctx.in_header_count < LUA_PLUGIN_MAX_HEADERS && json_is_string(hv)) {
                    snprintf(resp_ctx.in_headers[resp_ctx.in_header_count].key,
                             sizeof(resp_ctx.in_headers[0].key),
                             "%s",
                             hk);
                    snprintf(resp_ctx.in_headers[resp_ctx.in_header_count].value,
                             sizeof(resp_ctx.in_headers[0].value),
                             "%s",
                             json_string_value(hv));
                    resp_ctx.in_header_count++;
                }
            }
        }

        lua_bind_resp(L, &resp_ctx);
        res = lua_execute_single_script(script_code, "on_response", L, exec_err, sizeof(exec_err));
    }

    /* Extract captured test logs */
    json_t* logs_arr = json_array();
    lua_getfield(L, LUA_REGISTRYINDEX, REG_LOG_ARR_KEY);
    if (lua_istable(L, -1)) {
        lua_len(L, -1);
        lua_Integer nlogs = lua_tointeger(L, -1);
        lua_pop(L, 1);
        for (lua_Integer i = 1; i <= nlogs; i++) {
            lua_rawgeti(L, -1, i);
            const char* log_str = lua_tostring(L, -1);
            if (log_str) {
                json_array_append_new(logs_arr, json_string(log_str));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_close(L);

    json_t* out = json_object();
    json_object_set_new(out, "status", json_string(res == LUA_EXEC_ERROR ? "error" : "ok"));
    json_object_set_new(out, "success", json_boolean(res == LUA_EXEC_OK));
    json_object_set_new(out, "aborted", json_boolean(res == LUA_EXEC_ABORT));
    if (res == LUA_EXEC_ERROR) {
        json_object_set_new(
            out, "error", json_string(exec_err[0] != '\0' ? exec_err : "execution error"));
    }
    json_object_set_new(out, "logs", logs_arr);

    json_t* res_obj = json_object();
    if (res == LUA_EXEC_ABORT) {
        json_object_set_new(res_obj, "action", json_string("abort"));
        int         abort_st = !is_resp ? req_ctx.abort_status : resp_ctx.abort_status;
        const char* abort_msg = !is_resp ? (req_ctx.abort_body ? req_ctx.abort_body : "")
                                         : (resp_ctx.abort_body ? resp_ctx.abort_body : "");
        json_object_set_new(res_obj, "abort_status", json_integer(abort_st));
        json_object_set_new(res_obj, "abort_body", json_string(abort_msg));
        json_object_set_new(out, "abort_status", json_integer(abort_st));
        json_object_set_new(out, "abort_reason", json_string(abort_msg));
    } else if (res == LUA_EXEC_ERROR) {
        json_object_set_new(res_obj, "action", json_string("error"));
        json_object_set_new(res_obj, "error", json_string(exec_err));
    } else {
        json_object_set_new(res_obj, "action", json_string("continue"));
    }

    if (!is_resp) {
        json_t* req_obj = json_object();
        json_object_set_new(req_obj,
                            "model",
                            json_string(req_ctx.model_rewritten
                                            ? req_ctx.new_model
                                            : (req_ctx.model ? req_ctx.model : "")));
        const char*  final_body = req_ctx.new_body ? req_ctx.new_body : req_ctx.raw_body;
        json_error_t bjerr;
        json_t*      pb = json_loads(final_body ? final_body : "{}", 0, &bjerr);
        if (pb) {
            json_object_set_new(req_obj, "body", pb);
        } else {
            json_object_set_new(req_obj, "body", json_string(final_body ? final_body : ""));
        }

        json_t* h_obj = json_object();
        for (size_t i = 0; i < req_ctx.in_header_count; i++) {
            json_object_set_new(
                h_obj, req_ctx.in_headers[i].key, json_string(req_ctx.in_headers[i].value));
        }
        for (size_t i = 0; i < req_ctx.out_header_count; i++) {
            json_object_set_new(
                h_obj, req_ctx.out_headers[i].key, json_string(req_ctx.out_headers[i].value));
        }
        json_object_set_new(req_obj, "headers", h_obj);
        json_object_set(out, "request", req_obj);
        json_object_set_new(res_obj, "request", req_obj);
    } else {
        json_t* resp_obj = json_object();
        int     final_st = resp_ctx.new_status != 0 ? resp_ctx.new_status : resp_ctx.status;
        json_object_set_new(resp_obj, "status", json_integer(final_st));
        const char*  final_body = resp_ctx.new_body ? resp_ctx.new_body : resp_ctx.raw_body;
        json_error_t bjerr;
        json_t*      pb = json_loads(final_body ? final_body : "{}", 0, &bjerr);
        if (pb) {
            json_object_set_new(resp_obj, "body", pb);
        } else {
            json_object_set_new(resp_obj, "body", json_string(final_body ? final_body : ""));
        }

        json_t* h_obj = json_object();
        for (size_t i = 0; i < resp_ctx.in_header_count; i++) {
            json_object_set_new(
                h_obj, resp_ctx.in_headers[i].key, json_string(resp_ctx.in_headers[i].value));
        }
        for (size_t i = 0; i < resp_ctx.out_header_count; i++) {
            json_object_set_new(
                h_obj, resp_ctx.out_headers[i].key, json_string(resp_ctx.out_headers[i].value));
        }
        json_object_set_new(resp_obj, "headers", h_obj);
        json_object_set(out, "response", resp_obj);
        json_object_set_new(res_obj, "response", resp_obj);
    }

    json_object_set_new(out, "result", res_obj);

    *out_result_json = json_dumps(out, JSON_COMPACT);
    *out_result_len = strlen(*out_result_json);

    json_decref(out);
    json_decref(mock_root);
    if (body_str) {
        free(body_str);
    }
    lua_req_ctx_cleanup(&req_ctx);
    lua_resp_ctx_cleanup(&resp_ctx);

    return 0;
}
