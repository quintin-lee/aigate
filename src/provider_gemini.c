/** @file provider_gemini.c
 *  @brief Google Gemini provider adapter (Plan 3, Tasks 3 & 4).
 */
#include "provider_gemini.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
provider_gemini_supports(const char* provider)
{
    return provider != NULL &&
           (strcmp(provider, "gemini") == 0 || strcmp(provider, "google") == 0);
}

static bool
adapter_gemini_supports(const char* provider)
{
    return provider_gemini_supports(provider) != 0;
}

int
provider_gemini_build(const model_rec_t* route,
                      const char*        in_body,
                      char*              url_out,
                      size_t             url_cap,
                      const char*        extra_headers[4][2],
                      int*               n_extra_headers,
                      char**             out_body,
                      size_t*            out_body_len)
{
    /* Endpoint handling: default to google api url if empty */
    const char* ep = route->endpoint;
    if (ep == NULL || ep[0] == '\0' || strcmp(ep, "/") == 0) {
        ep = "https://generativelanguage.googleapis.com";
    }
    char base_ep[512];
    snprintf(base_ep, sizeof base_ep, "%s", ep);
    size_t elen = strlen(base_ep);
    if (elen > 0 && base_ep[elen - 1] == '/') {
        base_ep[elen - 1] = '\0';
    }

    /* Auth header: x-goog-api-key */
    int n_hdrs = 0;
    if (route->upstream_key[0] != '\0') {
        extra_headers[n_hdrs][0] = "x-goog-api-key";
        extra_headers[n_hdrs][1] = route->upstream_key;
        n_hdrs++;
    }
    *n_extra_headers = n_hdrs;

    /* Parse inbound OpenAI request */
    json_t* in_req = NULL;
    if (in_body != NULL && in_body[0] != '\0') {
        in_req = json_loads(in_body, 0, NULL);
    }
    if (in_req == NULL) {
        in_req = json_object();
    }

    /* Check if streaming */
    json_t* jstream = json_object_get(in_req, "stream");
    bool is_streaming = (jstream != NULL && json_is_true(jstream));

    /* Target URL */
    if (is_streaming) {
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:streamGenerateContent?alt=sse",
                 base_ep, route->name);
    } else {
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:generateContent",
                 base_ep, route->name);
    }

    json_t* out_req = json_object();
    json_t* contents = json_array();

    /* Process messages */
    json_t* msgs = json_object_get(in_req, "messages");
    char    system_buf[8192];
    size_t  system_len = 0;
    system_buf[0] = '\0';

    if (msgs != NULL && json_is_array(msgs)) {
        size_t idx;
        json_t* m;
        json_array_foreach(msgs, idx, m) {
            json_t* jrole = json_object_get(m, "role");
            json_t* jcontent = json_object_get(m, "content");
            const char* role = (jrole && json_is_string(jrole)) ? json_string_value(jrole) : "user";
            const char* content = (jcontent && json_is_string(jcontent)) ? json_string_value(jcontent) : "";

            if (strcmp(role, "system") == 0) {
                size_t clen = strlen(content);
                if (clen > 0) {
                    if (system_len > 0 && system_len + 2 < sizeof(system_buf)) {
                        memcpy(system_buf + system_len, "\n\n", 2);
                        system_len += 2;
                    }
                    if (system_len + clen < sizeof(system_buf)) {
                        memcpy(system_buf + system_len, content, clen);
                        system_len += clen;
                        system_buf[system_len] = '\0';
                    }
                }
            } else {
                const char* gemini_role = (strcmp(role, "assistant") == 0) ? "model" : "user";
                json_t* entry = json_object();
                json_object_set_new(entry, "role", json_string(gemini_role));
                json_t* parts = json_array();
                json_t* part = json_object();
                json_object_set_new(part, "text", json_string(content));
                json_array_append_new(parts, part);
                json_object_set_new(entry, "parts", parts);
                json_array_append_new(contents, entry);
            }
        }
    }

    if (system_len > 0) {
        json_t* sys_inst = json_object();
        json_t* sys_parts = json_array();
        json_t* sys_part = json_object();
        json_object_set_new(sys_part, "text", json_string(system_buf));
        json_array_append_new(sys_parts, sys_part);
        json_object_set_new(sys_inst, "parts", sys_parts);
        json_object_set_new(out_req, "systemInstruction", sys_inst);
    }

    json_object_set_new(out_req, "contents", contents);

    /* generationConfig */
    json_t* gen_cfg = json_object();
    json_t* jtemp = json_object_get(in_req, "temperature");
    if (jtemp != NULL && json_is_number(jtemp)) {
        json_object_set_new(gen_cfg, "temperature", json_real(json_number_value(jtemp)));
    }
    json_t* jmax = json_object_get(in_req, "max_tokens");
    if (jmax != NULL && json_is_integer(jmax)) {
        json_object_set_new(gen_cfg, "maxOutputTokens", json_integer(json_integer_value(jmax)));
    }
    json_t* jtop_p = json_object_get(in_req, "top_p");
    if (jtop_p != NULL && json_is_number(jtop_p)) {
        json_object_set_new(gen_cfg, "topP", json_real(json_number_value(jtop_p)));
    }
    json_t* jstop = json_object_get(in_req, "stop");
    if (jstop != NULL) {
        if (json_is_string(jstop)) {
            json_t* arr = json_array();
            json_array_append_new(arr, json_string(json_string_value(jstop)));
            json_object_set_new(gen_cfg, "stopSequences", arr);
        } else if (json_is_array(jstop)) {
            json_object_set(gen_cfg, "stopSequences", jstop);
        }
    }

    if (json_object_size(gen_cfg) > 0) {
        json_object_set_new(out_req, "generationConfig", gen_cfg);
    } else {
        json_decref(gen_cfg);
    }

    char* packed = json_dumps(out_req, JSON_COMPACT);
    json_decref(out_req);
    json_decref(in_req);

    if (packed == NULL) {
        return -1;
    }
    *out_body = packed;
    if (out_body_len != NULL) {
        *out_body_len = strlen(packed);
    }
    return 0;
}

static const char*
map_gemini_finish_reason(const char* reason)
{
    if (reason == NULL || reason[0] == '\0') {
        return "stop";
    }
    if (strcmp(reason, "STOP") == 0) {
        return "stop";
    }
    if (strcmp(reason, "MAX_TOKENS") == 0) {
        return "length";
    }
    if (strcmp(reason, "SAFETY") == 0 || strcmp(reason, "RECITATION") == 0) {
        return "content_filter";
    }
    return "stop";
}

int
provider_gemini_resp_to_openai(const char* gemini_resp,
                               const char* req_model,
                               char**      out_openai,
                               size_t*     out_openai_len,
                               long*       out_ptok,
                               long*       out_ctok)
{
    if (out_ptok) *out_ptok = 0;
    if (out_ctok) *out_ctok = 0;
    *out_openai = NULL;
    *out_openai_len = 0;

    if (gemini_resp == NULL || gemini_resp[0] == '\0') {
        return -1;
    }

    json_t* root = json_loads(gemini_resp, 0, NULL);
    if (root == NULL) {
        return -1;
    }

    /* Check for upstream error response */
    json_t* jerr = json_object_get(root, "error");
    if (jerr != NULL && json_is_object(jerr)) {
        json_t* jmsg = json_object_get(jerr, "message");
        json_t* jcode = json_object_get(jerr, "code");
        const char* msg = (jmsg && json_is_string(jmsg)) ? json_string_value(jmsg) : "unknown upstream error";
        int code = (jcode && json_is_integer(jcode)) ? (int)json_integer_value(jcode) : 400;

        json_t* err_root = json_object();
        json_t* err_obj = json_object();
        json_object_set_new(err_obj, "message", json_string(msg));
        json_object_set_new(err_obj, "type", json_string("invalid_request_error"));
        json_object_set_new(err_obj, "code", json_integer(code));
        json_object_set_new(err_root, "error", err_obj);

        char* packed = json_dumps(err_root, JSON_COMPACT);
        json_decref(err_root);
        json_decref(root);
        if (packed == NULL) {
            return -1;
        }
        *out_openai = packed;
        *out_openai_len = strlen(packed);
        return 0;
    }

    /* Extract content from candidates[0].content.parts[0].text */
    const char* text = "";
    const char* finish_reason = "stop";

    json_t* candidates = json_object_get(root, "candidates");
    if (candidates != NULL && json_is_array(candidates) && json_array_size(candidates) > 0) {
        json_t* c0 = json_array_get(candidates, 0);
        json_t* content = json_object_get(c0, "content");
        if (content != NULL && json_is_object(content)) {
            json_t* parts = json_object_get(content, "parts");
            if (parts != NULL && json_is_array(parts) && json_array_size(parts) > 0) {
                json_t* p0 = json_array_get(parts, 0);
                json_t* jtext = json_object_get(p0, "text");
                if (jtext != NULL && json_is_string(jtext)) {
                    text = json_string_value(jtext);
                }
            }
        }
        json_t* jfinish = json_object_get(c0, "finishReason");
        if (jfinish != NULL && json_is_string(jfinish)) {
            finish_reason = map_gemini_finish_reason(json_string_value(jfinish));
        }
    }

    /* Usage metadata */
    long ptok = 0, ctok = 0;
    json_t* usage = json_object_get(root, "usageMetadata");
    if (usage != NULL && json_is_object(usage)) {
        json_t* jp = json_object_get(usage, "promptTokenCount");
        json_t* jc = json_object_get(usage, "candidatesTokenCount");
        if (jp && json_is_integer(jp)) ptok = json_integer_value(jp);
        if (jc && json_is_integer(jc)) ctok = json_integer_value(jc);
    }
    if (out_ptok) *out_ptok = ptok;
    if (out_ctok) *out_ctok = ctok;

    /* Build OpenAI format response */
    json_t* oai = json_object();
    char id[64];
    snprintf(id, sizeof id, "chatcmpl-gemini-%ld", (long)time(NULL));
    json_object_set_new(oai, "id", json_string(id));
    json_object_set_new(oai, "object", json_string("chat.completion"));
    json_object_set_new(oai, "created", json_integer((int64_t)time(NULL)));
    json_object_set_new(oai, "model", json_string(req_model != NULL ? req_model : "gemini"));

    json_t* choices = json_array();
    json_t* choice = json_object();
    json_object_set_new(choice, "index", json_integer(0));

    json_t* msg = json_object();
    json_object_set_new(msg, "role", json_string("assistant"));
    json_object_set_new(msg, "content", json_string(text));
    json_object_set_new(choice, "message", msg);
    json_object_set_new(choice, "finish_reason", json_string(finish_reason));
    json_array_append_new(choices, choice);
    json_object_set_new(oai, "choices", choices);

    json_t* usg = json_object();
    json_object_set_new(usg, "prompt_tokens", json_integer(ptok));
    json_object_set_new(usg, "completion_tokens", json_integer(ctok));
    json_object_set_new(usg, "total_tokens", json_integer(ptok + ctok));
    json_object_set_new(oai, "usage", usg);

    char* packed = json_dumps(oai, JSON_COMPACT);
    json_decref(oai);
    json_decref(root);

    if (packed == NULL) {
        return -1;
    }
    *out_openai = packed;
    *out_openai_len = strlen(packed);
    return 0;
}

/* ------------------------------------------------------------ streaming bridge */

typedef struct gemini_bridge {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    char                 line_buf[4096];
    size_t               line_len;
    char                 model[64];
    char                 msg_id[64];
    char                 finish_reason[32];
    long                 prompt_tokens;
    long                 completion_tokens;
    bool                 done_emitted;
} gemini_bridge_t;

static stream_bridge_t*
gemini_bridge_new(aigate_response_ctx* rc, const char* model)
{
    gemini_bridge_t* b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->rc = rc;
    snprintf(b->model, sizeof b->model, "%s", model ? model : "gemini");
    snprintf(b->msg_id, sizeof b->msg_id, "chatcmpl-gemini-%ld", (long)time(NULL));
    snprintf(b->finish_reason, sizeof b->finish_reason, "stop");
    return (stream_bridge_t*)b;
}

static int
gemini_bridge_send_chunk(gemini_bridge_t* b, const char* str)
{
    if (b->rc == NULL || b->rc->write == NULL) {
        return 0;
    }
    if (!b->headers_sent) {
        b->rc->status = 200;
        if (b->rc->set_header != NULL) {
            b->rc->set_header(b->rc->impl, "Content-Type", "text/event-stream; charset=utf-8");
            b->rc->set_header(b->rc->impl, "Cache-Control", "no-cache");
            b->rc->set_header(b->rc->impl, "Connection", "keep-alive");
        }
        b->headers_sent = true;
        b->rc->headers_sent = true;
    }
    return b->rc->write(b->rc->impl, str, strlen(str), false);
}

static void
gemini_bridge_process_line(gemini_bridge_t* b, const char* line)
{
    const char* d = strstr(line, "data:");
    if (d == NULL) {
        return;
    }
    d += 5;
    while (*d == ' ' || *d == '\t') {
        d++;
    }
    if (*d == '\0') {
        return;
    }

    json_t* root = json_loads(d, 0, NULL);
    if (root == NULL) {
        return;
    }

    /* Update tokens if present */
    json_t* usage = json_object_get(root, "usageMetadata");
    if (usage != NULL && json_is_object(usage)) {
        json_t* jp = json_object_get(usage, "promptTokenCount");
        json_t* jc = json_object_get(usage, "candidatesTokenCount");
        if (jp && json_is_integer(jp)) b->prompt_tokens = json_integer_value(jp);
        if (jc && json_is_integer(jc)) b->completion_tokens = json_integer_value(jc);
    }

    /* Extract delta text */
    const char* delta_text = NULL;
    json_t* candidates = json_object_get(root, "candidates");
    if (candidates != NULL && json_is_array(candidates) && json_array_size(candidates) > 0) {
        json_t* c0 = json_array_get(candidates, 0);
        json_t* content = json_object_get(c0, "content");
        if (content != NULL && json_is_object(content)) {
            json_t* parts = json_object_get(content, "parts");
            if (parts != NULL && json_is_array(parts) && json_array_size(parts) > 0) {
                json_t* p0 = json_array_get(parts, 0);
                json_t* jt = json_object_get(p0, "text");
                if (jt && json_is_string(jt)) {
                    delta_text = json_string_value(jt);
                }
            }
        }
        json_t* jfinish = json_object_get(c0, "finishReason");
        if (jfinish != NULL && json_is_string(jfinish)) {
            snprintf(b->finish_reason, sizeof b->finish_reason, "%s",
                     map_gemini_finish_reason(json_string_value(jfinish)));
        }
    }

    if (delta_text != NULL && delta_text[0] != '\0') {
        json_t* chunk = json_object();
        json_object_set_new(chunk, "id", json_string(b->msg_id));
        json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
        json_object_set_new(chunk, "created", json_integer((int64_t)time(NULL)));
        json_object_set_new(chunk, "model", json_string(b->model));

        json_t* choices = json_array();
        json_t* choice = json_object();
        json_object_set_new(choice, "index", json_integer(0));

        json_t* delta = json_object();
        json_object_set_new(delta, "content", json_string(delta_text));
        json_object_set_new(choice, "delta", delta);
        json_object_set_new(choice, "finish_reason", json_null());
        json_array_append_new(choices, choice);
        json_object_set_new(chunk, "choices", choices);

        char* packed = json_dumps(chunk, JSON_COMPACT);
        json_decref(chunk);
        if (packed != NULL) {
            char sse_line[4096];
            snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
            free(packed);
            gemini_bridge_send_chunk(b, sse_line);
        }
    }

    json_decref(root);
}

static int
gemini_stream_bridge_feed(void* bridge, const void* chunk, size_t len)
{
    gemini_bridge_t* b = bridge;
    const char* p = chunk;
    const char* end = p + len;

    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (b->line_len + seg < sizeof(b->line_buf)) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                while (b->line_len > 0 &&
                       (b->line_buf[b->line_len - 1] == '\r' || b->line_buf[b->line_len - 1] == ' ')) {
                    b->line_len--;
                }
                b->line_buf[b->line_len] = '\0';
                gemini_bridge_process_line(b, b->line_buf);
            }
            b->line_len = 0;
            p = nl + 1;
        } else {
            size_t seg = (size_t)(end - p);
            if (b->line_len + seg < sizeof(b->line_buf) - 1) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                b->line_buf[b->line_len] = '\0';
            } else {
                b->line_len = 0;
            }
            p = end;
        }
    }
    return 0;
}

static int
gemini_stream_bridge_finish(stream_bridge_t* bridge)
{
    gemini_bridge_t* b = (gemini_bridge_t*)bridge;
    if (!b->done_emitted) {
        /* Emit final chunk with finish_reason */
        json_t* chunk = json_object();
        json_object_set_new(chunk, "id", json_string(b->msg_id));
        json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
        json_object_set_new(chunk, "created", json_integer((int64_t)time(NULL)));
        json_object_set_new(chunk, "model", json_string(b->model));

        json_t* choices = json_array();
        json_t* choice = json_object();
        json_object_set_new(choice, "index", json_integer(0));
        json_object_set_new(choice, "delta", json_object());
        json_object_set_new(choice, "finish_reason", json_string(b->finish_reason[0] ? b->finish_reason : "stop"));
        json_array_append_new(choices, choice);
        json_object_set_new(chunk, "choices", choices);

        char* packed = json_dumps(chunk, JSON_COMPACT);
        json_decref(chunk);
        if (packed != NULL) {
            char sse_line[1024];
            snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
            free(packed);
            gemini_bridge_send_chunk(b, sse_line);
        }

        gemini_bridge_send_chunk(b, "data: [DONE]\n\n");
        b->done_emitted = true;
    }
    if (b->rc->write != NULL) {
        b->rc->write(b->rc->impl, "", 0, true);
    }
    return 0;
}

static bool
gemini_stream_bridge_headers_sent(stream_bridge_t* bridge)
{
    gemini_bridge_t* b = (gemini_bridge_t*)bridge;
    return b->headers_sent;
}

static void
gemini_stream_bridge_get_tokens(stream_bridge_t* bridge,
                                long*            out_ptok,
                                long*            out_ctok,
                                long*            out_cached_tok)
{
    gemini_bridge_t* b = (gemini_bridge_t*)bridge;
    if (out_ptok) *out_ptok = b->prompt_tokens;
    if (out_ctok) *out_ctok = b->completion_tokens;
    if (out_cached_tok) *out_cached_tok = 0;
}

static void
gemini_stream_bridge_free(stream_bridge_t* bridge)
{
    free(bridge);
}

/* ------------------------------------------------------------ adapter export */

static int
gemini_build_chat(const model_rec_t* route,
                  const char*        in_body,
                  char*              url_out,
                  size_t             url_cap,
                  const char*        extra_headers[4][2],
                  int*               n_extra_headers,
                  char**             out_body,
                  size_t*            out_body_len)
{
    return provider_gemini_build(
        route, in_body, url_out, url_cap, extra_headers, n_extra_headers, out_body, out_body_len);
}

static int
gemini_parse_chat_response(const char* raw_body,
                           size_t      raw_len,
                           const char* model,
                           int*        http_status,
                           char**      out_body,
                           size_t*     out_len,
                           long*       out_ptok,
                           long*       out_ctok,
                           long*       out_cached_tok)
{
    (void)raw_len;
    if (out_cached_tok) *out_cached_tok = 0;
    *http_status = 200;
    return provider_gemini_resp_to_openai(raw_body, model, out_body, out_len, out_ptok, out_ctok);
}

const provider_adapter_t g_provider_gemini = {
    .name = "gemini",
    .supports = adapter_gemini_supports,
    .build_chat = gemini_build_chat,
    .parse_chat_response = gemini_parse_chat_response,
    .stream_bridge_new = gemini_bridge_new,
    .stream_bridge_feed = gemini_stream_bridge_feed,
    .stream_bridge_finish = gemini_stream_bridge_finish,
    .stream_bridge_headers_sent = gemini_stream_bridge_headers_sent,
    .stream_bridge_get_tokens = gemini_stream_bridge_get_tokens,
    .stream_bridge_free = gemini_stream_bridge_free,
    .build_embeddings = NULL,
    .parse_embeddings_response = NULL,
};
