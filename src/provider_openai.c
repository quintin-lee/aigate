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
    return provider != NULL && (strcmp(provider, "openai") == 0 ||
                                strcmp(provider, "ollama") == 0 ||
                                strcmp(provider, "azure") == 0 ||
                                strcmp(provider, "deepseek") == 0 ||
                                strcmp(provider, "siliconflow") == 0 ||
                                strcmp(provider, "vllm") == 0);
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
    if (out_ptok) *out_ptok = 0;
    if (out_ctok) *out_ctok = 0;
    if (out_cached_tok) *out_cached_tok = 0;
    *http_status = 200;

    if (raw_body != NULL && raw_len > 0) {
        json_t* root = json_loads(raw_body, 0, NULL);
        if (root != NULL) {
            json_t* jusage = json_object_get(root, "usage");
            if (jusage != NULL && json_is_object(jusage)) {
                json_t* jp = json_object_get(jusage, "prompt_tokens");
                json_t* jc = json_object_get(jusage, "completion_tokens");
                if (json_is_integer(jp) && out_ptok) *out_ptok = json_integer_value(jp);
                if (json_is_integer(jc) && out_ctok) *out_ctok = json_integer_value(jc);

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
    char                 line_buf[4096];
    size_t               line_len;
    long                 prompt_tokens;
    long                 completion_tokens;
    long                 cached_tokens;
} openai_bridge_t;

static stream_bridge_t*
openai_bridge_new(aigate_response_ctx* rc, const char* model)
{
    (void)model;
    openai_bridge_t* b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->rc = rc;
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
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jp = json_object_get(jusage, "prompt_tokens");
        json_t* jc = json_object_get(jusage, "completion_tokens");
        if (json_is_integer(jp)) acc->prompt_tokens = json_integer_value(jp);
        if (json_is_integer(jc)) acc->completion_tokens = json_integer_value(jc);

        json_t* jch = json_object_get(jusage, "prompt_cache_hit_tokens");
        if (json_is_integer(jch)) {
            acc->cached_tokens = json_integer_value(jch);
        } else {
            json_t* jdet = json_object_get(jusage, "prompt_tokens_details");
            if (jdet != NULL && json_is_object(jdet)) {
                json_t* jcd = json_object_get(jdet, "cached_tokens");
                if (json_is_integer(jcd)) acc->cached_tokens = json_integer_value(jcd);
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
    if (out_ptok) *out_ptok = acc->prompt_tokens;
    if (out_ctok) *out_ctok = acc->completion_tokens;
    if (out_cached_tok) *out_cached_tok = acc->cached_tokens;
}

static void
openai_bridge_free(stream_bridge_t* b)
{
    free(b);
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
    .build_embeddings = NULL,
    .parse_embeddings_response = NULL,
};

