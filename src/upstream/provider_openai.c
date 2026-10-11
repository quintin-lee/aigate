/** @file provider_openai.c
 *  @brief OpenAI-compatible request builder (see provider_openai.h). */
#include "provider_openai.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

int
provider_openai_supports(const char* provider)
{
    return provider != NULL &&
           (strcmp(provider, "openai") == 0 || strcmp(provider, "ollama") == 0 ||
            strcmp(provider, "azure") == 0 || strcmp(provider, "deepseek") == 0 ||
            strcmp(provider, "siliconflow") == 0 || strcmp(provider, "vllm") == 0);
}

int
provider_openai_build(const model_rec_t* route,
                      const char*        up_path,
                      const char*        in_body,
                      char*              url_out,
                      size_t             url_cap,
                      char**             out_body,
                      size_t*            out_body_len)
{
    /* start from the request body object (may be empty) */
    json_t* req = json_object();
    if (in_body != NULL && in_body[0] != '\0') {
        json_t* parsed = json_loads(in_body, 0, NULL);
        if (parsed == NULL) {
            /* malformed request JSON: treat as empty; the upstream will reject it */
            AIGATE_LOG_WARN("request body for model %s is not valid JSON", route->name);
        } else {
            json_object_update(req, parsed);
            json_decref(parsed);
        }
    }
    /* merge default_params under the request body (request wins on conflict) */
    if (route->default_params_json[0] != '\0') {
        json_t* defaults = json_loads(route->default_params_json, 0, NULL);
        if (defaults == NULL) {
            AIGATE_LOG_WARN("bad default_params for model %s", route->name);
        } else {
            json_t* merged = json_object();
            json_object_update(merged, defaults); /* defaults first */
            json_object_update(merged, req);      /* request wins */
            json_decref(req);
            json_decref(defaults);
            req = merged;
        }
    }

    /* URL: endpoint + up_path; for azure, append ?api-version= (from params). */
    const char* ver = "";
    if (strcmp(route->provider, "azure") == 0) {
        json_t* jv = json_object_get(req, "api-version");
        if (jv != NULL && json_is_string(jv)) {
            ver = json_string_value(jv);
        }
    }

    char packed_url[1024];
    if (ver[0] != '\0') {
        snprintf(packed_url,
                 sizeof packed_url,
                 "%s%s?api-version=%s",
                 route->endpoint,
                 up_path != NULL ? up_path : "",
                 ver);
        json_object_del(req, "api-version");
    } else {
        snprintf(
            packed_url, sizeof packed_url, "%s%s", route->endpoint, up_path != NULL ? up_path : "");
    }
    snprintf(url_out, url_cap, "%s", packed_url);

    /* If stream is requested, auto-inject stream_options: {"include_usage": true} */
    json_t* st = json_object_get(req, "stream");
    if (st != NULL && json_is_true(st)) {
        json_t* so = json_object_get(req, "stream_options");
        if (so == NULL) {
            so = json_pack("{s:b}", "include_usage", 1);
            json_object_set_new(req, "stream_options", so);
        } else if (json_is_object(so)) {
            json_object_set_new(so, "include_usage", json_true());
        }
    }

    char* packed = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (packed == NULL) {
        return -1;
    }
    *out_body = packed;
    if (out_body_len != NULL) {
        *out_body_len = strlen(packed);
    }
    return 0;
}

/* ------------------------------------------------------------ adapter impl */

/** @brief Adapter supports stub: delegates to provider_openai_supports. */
static bool
adapter_openai_supports(const char* provider)
{
    return provider_openai_supports(provider) != 0;
}

/** @brief Adapter build_chat stub: assembles the `/chat/completions` URL + passes the request body through (no extra headers).
 *  @return 0 on success; -1 on URL overflow / allocation failure. */
static int
openai_build_chat(const model_rec_t* route,
                  const char*        in_body,
                  char*              url_out,
                  size_t             url_cap,
                  const char*        extra_headers[4][2],
                  int*               n_extra_headers,
                  char**             out_body,
                  size_t*            out_body_len)
{
    (void)extra_headers;
    *n_extra_headers = 0;
    return provider_openai_build(
        route, "/chat/completions", in_body, url_out, url_cap, out_body, out_body_len);
}

/** @brief Adapter parse_chat_response stub: extracts usage tokens (prompt/completion/cached), then passes the response body through verbatim.
 *  @return 0 on success; -1 on allocation failure. */
static int
openai_parse_chat_response(const char* raw_body,
                           size_t      raw_len,
                           const char* model,
                           int*        http_status,
                           char**      out_body,
                           size_t*     out_len,
                           long*       out_ptok,
                           long*       out_ctok,
                           long*       out_cached_tok)
{
    (void)model;
    if (out_ptok) {
        *out_ptok = 0;
    }
    if (out_ctok) {
        *out_ctok = 0;
    }
    if (out_cached_tok) {
        *out_cached_tok = 0;
    }
    *http_status = 200;

    if (raw_body != NULL && raw_len > 0) {
        json_t* root = json_loads(raw_body, 0, NULL);
        if (root != NULL) {
            json_t* jusage = json_object_get(root, "usage");
            if (jusage != NULL && json_is_object(jusage)) {
                json_t* jp = json_object_get(jusage, "prompt_tokens");
                json_t* jc = json_object_get(jusage, "completion_tokens");
                if (json_is_integer(jp) && out_ptok) {
                    *out_ptok = json_integer_value(jp);
                }
                if (json_is_integer(jc) && out_ctok) {
                    *out_ctok = json_integer_value(jc);
                }

                /* DeepSeek prompt_cache_hit_tokens or OpenAI cached_tokens */
                json_t* jch = json_object_get(jusage, "prompt_cache_hit_tokens");
                if (json_is_integer(jch) && out_cached_tok) {
                    *out_cached_tok = json_integer_value(jch);
                } else {
                    json_t* jdet = json_object_get(jusage, "prompt_tokens_details");
                    if (jdet != NULL && json_is_object(jdet)) {
                        json_t* jcd = json_object_get(jdet, "cached_tokens");
                        if (json_is_integer(jcd) && out_cached_tok) {
                            *out_cached_tok = json_integer_value(jcd);
                        }
                    }
                }
            }
            json_decref(root);
        }
    }

    *out_body = malloc(raw_len + 1);
    if (*out_body == NULL) {
        return -1;
    }
    memcpy(*out_body, raw_body, raw_len);
    (*out_body)[raw_len] = '\0';
    *out_len = raw_len;
    return 0;
}

/** @brief OpenAI passthrough streaming bridge state machine (sends the SSE header only on the first packet). */
typedef struct {
    aigate_response_ctx* rc;           /**< Downstream response context (borrowed). */
    bool                 headers_sent; /**< Downstream headers already sent. */
    char                 line_buf
        [8192]; /**< SSE line buffer; lines over 8KB are truncated (P3-6), overlong deltas lose token counts. */
    size_t line_len;          /**< Line buffer bytes used. */
    char   model[128];        /**< Model name. */
    long   prompt_tokens;     /**< Accumulated prompt tokens. */
    long   completion_tokens; /**< Accumulated completion tokens. */
    long   cached_tokens;     /**< Accumulated cache-hit tokens. */
    long   reasoning_tokens;  /**< Accumulated reasoning tokens. */
} openai_bridge_t;            /**< OpenAI passthrough streaming bridge type (see above). */

/** @brief Create an OpenAI passthrough streaming bridge (sends the SSE header only on the first packet).
 *  @return The bridge; NULL on OOM. */
static stream_bridge_t*
openai_bridge_new(aigate_response_ctx* rc, const char* model)
{
    openai_bridge_t* b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->rc = rc;
    snprintf(b->model, sizeof b->model, "%s", model ? model : "");
    return (stream_bridge_t*)b;
}

/** @brief Parse `"usage"`-bearing SSE lines in the passthrough stream, accumulating prompt/completion/cached/reasoning tokens (input/output aliases supported). */
static void
openai_stream_process_line(openai_bridge_t* acc, const char* line)
{
    const char* d = strstr(line, "data:");
    if (d == NULL) {
        return;
    }
    const char* u = strstr(d, "\"usage\"");
    if (u == NULL) {
        return;
    }
    const char* jstart = strchr(d, '{');
    if (jstart == NULL) {
        return;
    }
    json_t* root = json_loads(jstart, 0, NULL);
    if (root == NULL) {
        return;
    }
    json_t* jusage = json_object_get(root, "usage");
    if (jusage == NULL) {
        json_t* jresp = json_object_get(root, "response");
        if (jresp != NULL && json_is_object(jresp)) {
            jusage = json_object_get(jresp, "usage");
        }
    }
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jp = json_object_get(jusage, "prompt_tokens");
        if (jp == NULL) {
            jp = json_object_get(jusage, "input_tokens");
        }
        json_t* jc = json_object_get(jusage, "completion_tokens");
        if (jc == NULL) {
            jc = json_object_get(jusage, "output_tokens");
        }
        if (json_is_integer(jp)) {
            acc->prompt_tokens = json_integer_value(jp);
        }
        if (json_is_integer(jc)) {
            acc->completion_tokens = json_integer_value(jc);
        }

        json_t* jch = json_object_get(jusage, "prompt_cache_hit_tokens");
        if (json_is_integer(jch)) {
            acc->cached_tokens = json_integer_value(jch);
        } else {
            json_t* jdet = json_object_get(jusage, "prompt_tokens_details");
            if (jdet == NULL) {
                jdet = json_object_get(jusage, "input_tokens_details");
            }
            if (jdet != NULL && json_is_object(jdet)) {
                json_t* jcd = json_object_get(jdet, "cached_tokens");
                if (json_is_integer(jcd)) {
                    acc->cached_tokens = json_integer_value(jcd);
                }
            }
        }

        json_t* jout_det = json_object_get(jusage, "completion_tokens_details");
        if (jout_det == NULL) {
            jout_det = json_object_get(jusage, "output_tokens_details");
        }
        if (jout_det != NULL && json_is_object(jout_det)) {
            json_t* jr = json_object_get(jout_det, "reasoning_tokens");
            if (json_is_integer(jr)) {
                acc->reasoning_tokens = json_integer_value(jr);
            }
        }
    }
    json_decref(root);
}

/** @brief Passthrough stream feed: sends the SSE header on the first packet, forwards chunks verbatim, and accumulates usage per line.
 *  @return 0 on success; -1 on downstream write failure. */
static int
openai_bridge_feed(void* bridge, const void* chunk, size_t len)
{
    openai_bridge_t* acc = bridge;
    if (!acc->headers_sent) {
        acc->rc->status = 200;
        if (acc->rc->set_header != NULL) {
            acc->rc->set_header(acc->rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
            acc->rc->set_header(acc->rc->impl, "Cache-Control", "no-cache");
            acc->rc->set_header(acc->rc->impl, "Connection", "keep-alive");
        }
        acc->headers_sent = true;
        acc->rc->headers_sent = true;
    }

    if (acc->rc->write != NULL && len > 0) {
        if (acc->rc->write(acc->rc->impl, chunk, len, false) != 0) {
            return -1;
        }
    }

    const char* p = chunk;
    const char* end = p + len;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (acc->line_len + seg < sizeof(acc->line_buf)) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';
                openai_stream_process_line(acc, acc->line_buf);
            } else {
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                acc->model[0] ? acc->model : "unknown");
            }
            acc->line_len = 0;
            p = nl + 1;
        } else {
            size_t seg = (size_t)(end - p);
            if (acc->line_len + seg < sizeof(acc->line_buf) - 1) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                acc->line_buf[acc->line_len] = '\0';
            } else {
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                acc->model[0] ? acc->model : "unknown");
                acc->line_len = 0;
            }
            p = end;
        }
    }
    return 0;
}

/** @brief Terminate the passthrough stream (empty write as the fin marker). @return Downstream write result. */
static int
openai_bridge_finish(stream_bridge_t* b)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    if (acc->rc->write != NULL) {
        return acc->rc->write(acc->rc->impl, "", 0, true);
    }
    return 0;
}

/** @brief Whether the passthrough stream has sent headers. */
static bool
openai_bridge_headers_sent(stream_bridge_t* b)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    return acc->headers_sent;
}

/** @brief Get the passthrough stream's accumulated prompt/completion/cached tokens (any out may be NULL). */
static void
openai_bridge_get_tokens(stream_bridge_t* b, long* out_ptok, long* out_ctok, long* out_cached_tok)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    if (out_ptok) {
        *out_ptok = acc->prompt_tokens;
    }
    if (out_ctok) {
        *out_ctok = acc->completion_tokens;
    }
    if (out_cached_tok) {
        *out_cached_tok = acc->cached_tokens;
    }
}

void
provider_openai_bridge_get_tokens(stream_bridge_t* b,
                                  long*            out_ptok,
                                  long*            out_ctok,
                                  long*            out_cached_tok,
                                  long*            out_reasoning_tok)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    if (out_ptok) {
        *out_ptok = acc->prompt_tokens;
    }
    if (out_ctok) {
        *out_ctok = acc->completion_tokens;
    }
    if (out_cached_tok) {
        *out_cached_tok = acc->cached_tokens;
    }
    if (out_reasoning_tok) {
        *out_reasoning_tok = acc->reasoning_tokens;
    }
}

/** @brief Free the passthrough streaming bridge. */
static void
openai_bridge_free(stream_bridge_t* b)
{
    free(b);
}

int
provider_openai_build_embeddings(const model_rec_t* route,
                                 const char*        in_body,
                                 char*              url_out,
                                 size_t             url_cap,
                                 const char*        extra_headers[4][2],
                                 int*               n_extra_headers,
                                 char**             out_body,
                                 size_t*            out_body_len)
{
    (void)extra_headers;
    *n_extra_headers = 0;

    const char* up_path = "/embeddings";
    size_t      elen = strlen(route->endpoint);
    bool        has_v1 = (strstr(route->endpoint, "/v1") != NULL);
    if (!has_v1) {
        up_path = "/v1/embeddings";
    }
    if (elen > 0 && route->endpoint[elen - 1] == '/') {
        if (up_path[0] == '/') {
            up_path++;
        }
    }

    return provider_openai_build(route, up_path, in_body, url_out, url_cap, out_body, out_body_len);
}

int
provider_openai_parse_embeddings(const char* raw_body,
                                 size_t      raw_len,
                                 const char* model,
                                 int*        http_status,
                                 char**      out_body,
                                 size_t*     out_len,
                                 long*       out_ptok)
{
    (void)model;
    if (out_ptok) {
        *out_ptok = 0;
    }
    *http_status = 200;

    if (raw_body != NULL && raw_len > 0) {
        json_t* root = json_loads(raw_body, 0, NULL);
        if (root != NULL) {
            json_t* jusage = json_object_get(root, "usage");
            if (jusage != NULL && json_is_object(jusage)) {
                json_t* jp = json_object_get(jusage, "prompt_tokens");
                if (json_is_integer(jp) && out_ptok) {
                    *out_ptok = json_integer_value(jp);
                } else {
                    json_t* jt = json_object_get(jusage, "total_tokens");
                    if (json_is_integer(jt) && out_ptok) {
                        *out_ptok = json_integer_value(jt);
                    }
                }
            }
            json_decref(root);
        }
    }

    *out_body = malloc(raw_len + 1);
    if (*out_body == NULL) {
        return -1;
    }
    memcpy(*out_body, raw_body, raw_len);
    (*out_body)[raw_len] = '\0';
    *out_len = raw_len;
    return 0;
}

/** Responses API auth header thread-local cache ("Bearer <key>", avoids per-request snprintf). */
static _Thread_local char s_responses_bearer_auth[2048];

int
provider_openai_build_responses(const model_rec_t* route,
                                const char*        in_body,
                                char*              url_out,
                                size_t             url_cap,
                                const char*        extra_headers[4][2],
                                int*               n_extra_headers,
                                char**             out_body,
                                size_t*            out_body_len)
{
    const char* up_path = "/responses";
    size_t      elen = strlen(route->endpoint);
    bool        has_v1 = (strstr(route->endpoint, "/v1") != NULL);
    if (!has_v1) {
        up_path = "/v1/responses";
    }
    if (elen > 0 && route->endpoint[elen - 1] == '/') {
        if (up_path[0] == '/') {
            up_path++;
        }
    }
    snprintf(url_out, url_cap, "%s%s", route->endpoint, up_path);

    int n_hdrs = 0;
    if (route->upstream_key[0] != '\0') {
        snprintf(s_responses_bearer_auth,
                 sizeof(s_responses_bearer_auth),
                 "Bearer %s",
                 route->upstream_key);
        extra_headers[n_hdrs][0] = "Authorization";
        extra_headers[n_hdrs][1] = s_responses_bearer_auth;
        n_hdrs++;
    }
    extra_headers[n_hdrs][0] = "Content-Type";
    extra_headers[n_hdrs][1] = "application/json";
    n_hdrs++;
    *n_extra_headers = n_hdrs;

    *out_body = strdup(in_body ? in_body : "");
    if (*out_body == NULL) {
        return -1;
    }
    if (out_body_len != NULL) {
        *out_body_len = strlen(*out_body);
    }
    return 0;
}

/** @brief Extract input/output/cached/reasoning tokens from a Responses API response (including nested `response.usage`); missing fields leave out values untouched. */
static void
extract_responses_usage_from_json(json_t* root,
                                  long*   out_input_tokens,
                                  long*   out_output_tokens,
                                  long*   out_cached_tokens,
                                  long*   out_reasoning_tokens)
{
    if (root == NULL || !json_is_object(root)) {
        return;
    }
    json_t* jusage = json_object_get(root, "usage");
    if (jusage == NULL || !json_is_object(jusage)) {
        json_t* jresp = json_object_get(root, "response");
        if (jresp != NULL && json_is_object(jresp)) {
            jusage = json_object_get(jresp, "usage");
        }
    }
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jin = json_object_get(jusage, "input_tokens");
        if (jin == NULL) {
            jin = json_object_get(jusage, "prompt_tokens");
        }
        if (json_is_integer(jin) && out_input_tokens) {
            *out_input_tokens = json_integer_value(jin);
        }

        json_t* jout = json_object_get(jusage, "output_tokens");
        if (jout == NULL) {
            jout = json_object_get(jusage, "completion_tokens");
        }
        if (json_is_integer(jout) && out_output_tokens) {
            *out_output_tokens = json_integer_value(jout);
        }

        /* cached tokens */
        json_t* jin_det = json_object_get(jusage, "input_tokens_details");
        if (jin_det == NULL) {
            jin_det = json_object_get(jusage, "prompt_tokens_details");
        }
        if (jin_det != NULL && json_is_object(jin_det)) {
            json_t* jcached = json_object_get(jin_det, "cached_tokens");
            if (json_is_integer(jcached) && out_cached_tokens) {
                *out_cached_tokens = json_integer_value(jcached);
            }
        }

        /* reasoning tokens */
        json_t* jout_det = json_object_get(jusage, "output_tokens_details");
        if (jout_det == NULL) {
            jout_det = json_object_get(jusage, "completion_tokens_details");
        }
        if (jout_det != NULL && json_is_object(jout_det)) {
            json_t* jreasoning = json_object_get(jout_det, "reasoning_tokens");
            if (json_is_integer(jreasoning) && out_reasoning_tokens) {
                *out_reasoning_tokens = json_integer_value(jreasoning);
            }
        }
    }
}

int
provider_openai_parse_responses_usage(const char* body,
                                      size_t      len,
                                      long*       out_input_tokens,
                                      long*       out_output_tokens,
                                      long*       out_cached_tokens,
                                      long*       out_reasoning_tokens)
{
    if (out_input_tokens) {
        *out_input_tokens = 0;
    }
    if (out_output_tokens) {
        *out_output_tokens = 0;
    }
    if (out_cached_tokens) {
        *out_cached_tokens = 0;
    }
    if (out_reasoning_tokens) {
        *out_reasoning_tokens = 0;
    }

    if (body == NULL || len == 0) {
        return 0;
    }

    /* Check if SSE stream */
    if (strstr(body, "event:") != NULL || strstr(body, "data:") != NULL) {
        const char* p = body;
        const char* end = body + len;
        while (p < end) {
            const char* nl = memchr(p, '\n', (size_t)(end - p));
            size_t      line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (line_len > 5 && strncmp(p, "data:", 5) == 0) {
                const char* jstart = memchr(p, '{', line_len);
                if (jstart != NULL) {
                    size_t       jlen = line_len - (size_t)(jstart - p);
                    json_error_t err;
                    json_t*      root = json_loadb(jstart, jlen, 0, &err);
                    if (root != NULL) {
                        extract_responses_usage_from_json(root,
                                                          out_input_tokens,
                                                          out_output_tokens,
                                                          out_cached_tokens,
                                                          out_reasoning_tokens);
                        json_decref(root);
                    }
                }
            }
            p = nl ? nl + 1 : end;
        }
        return 0;
    }

    /* Non-streaming */
    json_error_t err;
    json_t*      root = json_loadb(body, len, 0, &err);
    if (root != NULL) {
        extract_responses_usage_from_json(
            root, out_input_tokens, out_output_tokens, out_cached_tokens, out_reasoning_tokens);
        json_decref(root);
        return 0;
    }
    return 0;
}

int
provider_openai_resp_to_anthropic(const char* oai_resp,
                                  const char* req_model,
                                  char**      out_anthropic,
                                  size_t*     out_anthropic_len,
                                  long*       out_ptok,
                                  long*       out_ctok)
{
    if (out_ptok) {
        *out_ptok = 0;
    }
    if (out_ctok) {
        *out_ctok = 0;
    }
    if (out_anthropic == NULL) {
        return -1;
    }
    *out_anthropic = NULL;
    if (out_anthropic_len != NULL) {
        *out_anthropic_len = 0;
    }

    if (oai_resp == NULL) {
        return -1;
    }

    json_error_t err;
    json_t* root = json_loads(oai_resp, 0, &err);
    if (root == NULL) {
        return -1;
    }

    /* ID */
    const char* oai_id = "unknown";
    json_t* jid = json_object_get(root, "id");
    if (jid != NULL && json_is_string(jid)) {
        oai_id = json_string_value(jid);
    }
    char ant_id[128];
    if (strncmp(oai_id, "chatcmpl-", 9) == 0) {
        snprintf(ant_id, sizeof(ant_id), "msg_%s", oai_id + 9);
    } else {
        snprintf(ant_id, sizeof(ant_id), "msg_%s", oai_id);
    }

    /* Model */
    const char* model = req_model ? req_model : "unknown";
    json_t* jm = json_object_get(root, "model");
    if (jm != NULL && json_is_string(jm)) {
        model = json_string_value(jm);
    }

    /* Choices */
    json_t* choices = json_object_get(root, "choices");
    json_t* c0 = (choices != NULL && json_is_array(choices)) ? json_array_get(choices, 0) : NULL;
    const char* finish_reason = "end_turn";
    const char* reasoning_text = NULL;
    const char* content_text = NULL;
    json_t* tool_calls = NULL;

    if (c0 != NULL && json_is_object(c0)) {
        json_t* jfr = json_object_get(c0, "finish_reason");
        if (jfr != NULL && json_is_string(jfr)) {
            const char* fr = json_string_value(jfr);
            if (strcmp(fr, "length") == 0) {
                finish_reason = "max_tokens";
            } else if (strcmp(fr, "tool_calls") == 0) {
                finish_reason = "tool_use";
            } else {
                finish_reason = "end_turn";
            }
        }
        json_t* jmsg = json_object_get(c0, "message");
        if (jmsg != NULL && json_is_object(jmsg)) {
            json_t* jrc = json_object_get(jmsg, "reasoning_content");
            if (jrc != NULL && json_is_string(jrc)) {
                reasoning_text = json_string_value(jrc);
            }
            json_t* jc = json_object_get(jmsg, "content");
            if (jc != NULL && json_is_string(jc)) {
                content_text = json_string_value(jc);
            }
            tool_calls = json_object_get(jmsg, "tool_calls");
        }
    }

    /* Content array */
    json_t* ant_content = json_array();

    /* Thinking block (if present) */
    if (reasoning_text != NULL && reasoning_text[0] != '\0') {
        json_t* tb = json_object();
        json_object_set_new(tb, "type", json_string("thinking"));
        json_object_set_new(tb, "thinking", json_string(reasoning_text));
        json_array_append_new(ant_content, tb);
    }

    /* Text block (if present) */
    if (content_text != NULL && content_text[0] != '\0') {
        json_t* tb = json_object();
        json_object_set_new(tb, "type", json_string("text"));
        json_object_set_new(tb, "text", json_string(content_text));
        json_array_append_new(ant_content, tb);
    }

    /* Tool calls blocks (if present) */
    if (tool_calls != NULL && json_is_array(tool_calls)) {
        size_t ti;
        json_t* tc;
        json_array_foreach(tool_calls, ti, tc) {
            json_t* jtcid = json_object_get(tc, "id");
            json_t* jfn = json_object_get(tc, "function");
            if (jfn == NULL || !json_is_object(jfn)) {
                continue;
            }
            json_t* jname = json_object_get(jfn, "name");
            json_t* jargs = json_object_get(jfn, "arguments");
            const char* tcid = (jtcid && json_is_string(jtcid)) ? json_string_value(jtcid) : "";
            const char* name = (jname && json_is_string(jname)) ? json_string_value(jname) : "";
            const char* args_str = (jargs && json_is_string(jargs)) ? json_string_value(jargs) : "{}";

            json_t* input_obj = json_loads(args_str, 0, NULL);
            if (input_obj == NULL) {
                input_obj = json_object();
            }

            json_t* tub = json_object();
            json_object_set_new(tub, "type", json_string("tool_use"));
            json_object_set_new(tub, "id", json_string(tcid));
            json_object_set_new(tub, "name", json_string(name));
            json_object_set_new(tub, "input", input_obj);
            json_array_append_new(ant_content, tub);
        }
    }

    /* Usage */
    long ptok = 0, ctok = 0;
    json_t* jusage = json_object_get(root, "usage");
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jpin = json_object_get(jusage, "prompt_tokens");
        if (jpin == NULL) {
            jpin = json_object_get(jusage, "input_tokens");
        }
        if (jpin != NULL && json_is_integer(jpin)) {
            ptok = json_integer_value(jpin);
        }
        json_t* jcout = json_object_get(jusage, "completion_tokens");
        if (jcout == NULL) {
            jcout = json_object_get(jusage, "output_tokens");
        }
        if (jcout != NULL && json_is_integer(jcout)) {
            ctok = json_integer_value(jcout);
        }
    }
    if (out_ptok) {
        *out_ptok = ptok;
    }
    if (out_ctok) {
        *out_ctok = ctok;
    }

    /* Construct Anthropic response */
    json_t* ant = json_object();
    json_object_set_new(ant, "id", json_string(ant_id));
    json_object_set_new(ant, "type", json_string("message"));
    json_object_set_new(ant, "role", json_string("assistant"));
    json_object_set_new(ant, "model", json_string(model));
    json_object_set_new(ant, "content", ant_content);
    json_object_set_new(ant, "stop_reason", json_string(finish_reason));
    json_object_set_new(ant, "stop_sequence", json_null());

    json_t* usage_obj = json_object();
    json_object_set_new(usage_obj, "input_tokens", json_integer(ptok));
    json_object_set_new(usage_obj, "output_tokens", json_integer(ctok));
    json_object_set_new(ant, "usage", usage_obj);

    json_decref(root);

    char* packed = json_dumps(ant, JSON_COMPACT);
    json_decref(ant);
    if (packed == NULL) {
        return -1;
    }

    *out_anthropic = packed;
    if (out_anthropic_len != NULL) {
        *out_anthropic_len = strlen(packed);
    }
    return 0;
}

/** @brief OpenAI-compatible provider vtable instance (see the provider_adapter vtable). */
const provider_adapter_t g_provider_openai = {
    .name = "openai",
    .supports = adapter_openai_supports,
    .build_chat = openai_build_chat,
    .parse_chat_response = openai_parse_chat_response,
    .stream_bridge_new = openai_bridge_new,
    .stream_bridge_feed = openai_bridge_feed,
    .stream_bridge_finish = openai_bridge_finish,
    .stream_bridge_headers_sent = openai_bridge_headers_sent,
    .stream_bridge_get_tokens = openai_bridge_get_tokens,
    .stream_bridge_free = openai_bridge_free,
    .build_embeddings = provider_openai_build_embeddings,
    .parse_embeddings_response = provider_openai_parse_embeddings,
    .build_responses = provider_openai_build_responses,
    .parse_responses_response = provider_openai_parse_responses_usage,
};
