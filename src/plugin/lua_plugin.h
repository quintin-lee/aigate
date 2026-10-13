/**
 * @file lua_plugin.h
 * @brief High-performance embedded Lua script plugin engine for aigate.
 *
 * Provides dynamic scripting capabilities for request rewriting, custom validation,
 * parameter normalization, prompt injection, and response transformation.
 */

#ifndef AIGATE_LUA_PLUGIN_H
#define AIGATE_LUA_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of custom headers a plugin can inject or inspect. */
#define LUA_PLUGIN_MAX_HEADERS 32

/** Maximum execution instruction hook step count to prevent infinite loops. */
#define LUA_PLUGIN_MAX_INSTRUCTIONS 200000

/** Execution outcome status returned by plugin hooks. */
typedef enum {
    LUA_EXEC_OK = 0,    /**< Plugin succeeded, continue request pipeline. */
    LUA_EXEC_ABORT = 1, /**< Plugin requested early termination via aigate.abort(). */
    LUA_EXEC_ERROR = -1 /**< Plugin encountered a runtime or syntax error. */
} lua_exec_result_t;

/** Plugin record definition. */
typedef struct lua_plugin_def {
    int64_t id;               /**< Unique database primary key identifier. */
    char    name[64];         /**< Unique human-readable plugin identifier. */
    char    phase[32];        /**< "request", "response", or "both". */
    int     priority;         /**< Execution priority (ascending order; 1 before 100). */
    bool    enabled;          /**< Whether plugin is actively evaluated. */
    char    match_model[64];  /**< Wildcard model filter (e.g. "*", "gpt-*"). */
    char    description[256]; /**< Optional operator notes/description. */
    char*   script_code;      /**< Raw Lua source code (heap allocated). */
    int64_t created_at;       /**< UNIX timestamp of creation. */
    int64_t updated_at;       /**< UNIX timestamp of last modification. */
} lua_plugin_def_t;

/** Single HTTP header key-value pair. */
typedef struct lua_header_entry {
    char key[128];
    char value[512];
} lua_header_entry_t;

/** Inbound request execution context passed to/from on_request(req). */
typedef struct lua_req_ctx {
    const char* method;    /**< HTTP method (e.g. "POST"). */
    const char* path;      /**< HTTP path (e.g. "/v1/chat/completions"). */
    const char* client_ip; /**< Client IP address string. */
    const char* model;     /**< Initial model identifier. */

    char new_model[64];    /**< Populated if plugin rewrites target model. */
    bool model_rewritten;  /**< True if new_model is valid and should overwrite. */

    lua_header_entry_t in_headers[LUA_PLUGIN_MAX_HEADERS];  /**< Input request headers. */
    size_t             in_header_count;

    lua_header_entry_t out_headers[LUA_PLUGIN_MAX_HEADERS]; /**< Injected/modified headers. */
    size_t             out_header_count;

    const char* raw_body;                                   /**< Original request body. */
    size_t      raw_body_len;                               /**< Original request body length. */

    char*  new_body;     /**< Heap-allocated replacement body if modified by plugin. */
    size_t new_body_len; /**< Length of replacement body. */

    /* Abort fields populated if aigate.abort(status, body) was triggered */
    int    abort_status; /**< HTTP status code for early exit (e.g. 403, 400). */
    char*  abort_body;   /**< Heap-allocated response payload. */
    size_t abort_body_len;
} lua_req_ctx_t;

/** Outbound response execution context passed to/from on_response(resp). */
typedef struct lua_resp_ctx {
    int status;     /**< In/Out: HTTP status code. */
    int new_status; /**< Modified status if changed. */

    lua_header_entry_t in_headers[LUA_PLUGIN_MAX_HEADERS];
    size_t             in_header_count;

    lua_header_entry_t out_headers[LUA_PLUGIN_MAX_HEADERS];
    size_t             out_header_count;

    const char* raw_body;
    size_t      raw_body_len;

    char*  new_body; /**< Heap-allocated replacement response body. */
    size_t new_body_len;

    int    abort_status;
    char*  abort_body;
    size_t abort_body_len;
} lua_resp_ctx_t;

/** Opaque plugin manager instance. */
typedef struct lua_plugin_mgr lua_plugin_mgr_t;

/**
 * @brief Create a new plugin manager instance.
 * @return Pointer to initialized manager, or NULL on allocation failure.
 */
lua_plugin_mgr_t* lua_plugin_mgr_create(void);

/**
 * @brief Destroy a plugin manager and free all stored plugin definitions.
 * @param mgr Plugin manager instance.
 */
void lua_plugin_mgr_destroy(lua_plugin_mgr_t* mgr);

/**
 * @brief Set and atomically replace all active plugin definitions in the manager.
 * @param mgr Plugin manager instance.
 * @param defs Array of plugin definitions.
 * @param count Number of elements in defs.
 * @return 0 on success, negative error code on failure.
 */
int lua_plugin_mgr_set_plugins(lua_plugin_mgr_t* mgr, const lua_plugin_def_t* defs, size_t count);

/**
 * @brief Execute all enabled, matching 'request' plugins on an inbound request.
 * @param mgr Plugin manager instance.
 * @param ctx Request context to inspect and mutate.
 * @return LUA_EXEC_OK to continue pipeline, LUA_EXEC_ABORT on early exit, LUA_EXEC_ERROR on fault.
 */
lua_exec_result_t lua_plugin_mgr_exec_request(lua_plugin_mgr_t* mgr, lua_req_ctx_t* ctx);

/**
 * @brief Execute all enabled, matching 'response' plugins on an outbound response.
 * @param mgr Plugin manager instance.
 * @param model Model associated with the request (for wildcard pattern matching).
 * @param ctx Response context to inspect and mutate.
 * @return LUA_EXEC_OK to continue pipeline, LUA_EXEC_ABORT on early exit, LUA_EXEC_ERROR on fault.
 */
lua_exec_result_t
lua_plugin_mgr_exec_response(lua_plugin_mgr_t* mgr, const char* model, lua_resp_ctx_t* ctx);

/**
 * @brief Validate Lua script syntax without executing it.
 * @param script_code Lua source code to validate.
 * @param err_buf Buffer to store syntax error message if invalid.
 * @param err_buf_cap Capacity of err_buf.
 * @return 0 if syntax is valid, negative if syntax error.
 */
int lua_plugin_validate_syntax(const char* script_code, char* err_buf, size_t err_buf_cap);

/**
 * @brief Execute a dry-run test of a Lua script in an isolated sandbox with mock payload.
 *
 * @param script_code Raw Lua source code to test.
 * @param phase "request" or "response".
 * @param mock_payload_json JSON string containing mock headers, method, path, model, body.
 * @param out_result_json Output buffer populated with JSON result (status, headers, body, logs).
 * @param out_result_len Output buffer length.
 * @return 0 on successful dry run, negative error code on execution or syntax failure.
 */
int lua_plugin_test_dry_run(const char* script_code,
                            const char* phase,
                            const char* mock_payload_json,
                            char**      out_result_json,
                            size_t*     out_result_len);

/**
 * @brief Free resources in a lua_req_ctx_t allocated during execution.
 */
void lua_req_ctx_cleanup(lua_req_ctx_t* ctx);

/**
 * @brief Free resources in a lua_resp_ctx_t allocated during execution.
 */
void lua_resp_ctx_cleanup(lua_resp_ctx_t* ctx);

/**
 * @brief Free a single plugin definition's internal allocations.
 */
void lua_plugin_def_free(lua_plugin_def_t* def);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_LUA_PLUGIN_H */
