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

static bool
adapter_openai_supports(const char* provider)
{
    return provider_openai_supports(provider) != 0;
}

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

typedef struct {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    char                 line_buf
        [8192]; /* SSE lines >8KB are truncated (P3-6): long deltas beyond this lose token accounting */
    size_t line_len;
    char   model[128];
    long   prompt_tokens;
    long   completion_tokens;
    long   cached_tokens;
    long   reasoning_tokens;
} openai_bridge_t;

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

static int
openai_bridge_finish(stream_bridge_t* b)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    if (acc->rc->write != NULL) {
        return acc->rc->write(acc->rc->impl, "", 0, true);
    }
    return 0;
}

static bool
openai_bridge_headers_sent(stream_bridge_t* b)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    return acc->headers_sent;
}

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
        snprintf(s_responses_bearer_auth, sizeof(s_responses_bearer_auth), "Bearer %s", route->upstream_key);
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
    if (out_input_tokens) *out_input_tokens = 0;
    if (out_output_tokens) *out_output_tokens = 0;
    if (out_cached_tokens) *out_cached_tokens = 0;
    if (out_reasoning_tokens) *out_reasoning_tokens = 0;

    if (body == NULL || len == 0) {
        return 0;
    }

    /* Check if SSE stream */
    if (strstr(body, "event:") != NULL || strstr(body, "data:") != NULL) {
        const char* p = body;
        const char* end = body + len;
        while (p < end) {
            const char* nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (line_len > 5 && strncmp(p, "data:", 5) == 0) {
                const char* jstart = memchr(p, '{', line_len);
                if (jstart != NULL) {
                    size_t jlen = line_len - (size_t)(jstart - p);
                    json_error_t err;
                    json_t* root = json_loadb(jstart, jlen, 0, &err);
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
    json_t* root = json_loadb(body, len, 0, &err);
    if (root != NULL) {
        extract_responses_usage_from_json(root,
                                          out_input_tokens,
                                          out_output_tokens,
                                          out_cached_tokens,
                                          out_reasoning_tokens);
        json_decref(root);
        return 0;
    }
    return 0;
}

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
