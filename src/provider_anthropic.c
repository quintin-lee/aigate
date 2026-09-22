/** @file provider_anthropic.c
 *  @brief Anthropic Claude provider adapter (messages API & SSE bridge).
 *  SSE re-emit buffers are 8192 bytes (P3-6): event payloads longer than
 *  ~8KB are truncated with a warn; token accounting for that delta is lost.
 */
#include "provider_anthropic.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int
provider_anthropic_supports(const char* provider)
{
    return provider != NULL && strcmp(provider, "anthropic") == 0;
}

int
provider_anthropic_build(const model_rec_t* route,
                         const char*        in_body,
                         char*              url_out,
                         size_t             url_cap,
                         const char*        headers_kv[4][2],
                         int*               n_headers,
                         char**             out_body,
                         size_t*            out_body_len)
{
    /* URL: endpoint + /messages or /v1/messages */
    size_t elen = strlen(route->endpoint);
    if (elen >= 3 && strcmp(route->endpoint + elen - 3, "/v1") == 0) {
        snprintf(url_out, url_cap, "%s/messages", route->endpoint);
    } else if (elen >= 4 && strcmp(route->endpoint + elen - 4, "/v1/") == 0) {
        snprintf(url_out, url_cap, "%smessages", route->endpoint);
    } else {
        snprintf(url_out, url_cap, "%s/v1/messages", route->endpoint);
    }

    /* Headers: x-api-key, anthropic-version */
    static const char* s_anthropic_ver = "2023-06-01";
    static const char* s_hdr_version = "anthropic-version";
    static const char* s_hdr_apikey = "x-api-key";

    int n_hdrs = 0;
    if (route->upstream_key[0] != '\0') {
        headers_kv[n_hdrs][0] = s_hdr_apikey;
        headers_kv[n_hdrs][1] = route->upstream_key;
        n_hdrs++;
    }
    headers_kv[n_hdrs][0] = s_hdr_version;
    headers_kv[n_hdrs][1] = s_anthropic_ver;
    n_hdrs++;
    *n_headers = n_hdrs;

    /* Parse inbound OpenAI request */
    json_t* in_req = NULL;
    if (in_body != NULL && in_body[0] != '\0') {
        in_req = json_loads(in_body, 0, NULL);
    }
    if (in_req == NULL) {
        in_req = json_object();
    }

    json_t* out = json_object();

    /* Model: from request, or route name */
    const char* model_name = route->name;
    json_t*     jm = json_object_get(in_req, "model");
    if (jm != NULL && json_is_string(jm)) {
        model_name = json_string_value(jm);
    }
    json_object_set_new(out, "model", json_string(model_name));

    /* Extract system messages and non-system messages */
    json_t* msgs = json_object_get(in_req, "messages");
    char*   sys_buf = NULL;
    size_t  sys_len = 0;
    json_t* ant_msgs = json_array();

    if (msgs != NULL && json_is_array(msgs)) {
        size_t  idx;
        json_t* item;
        json_array_foreach(msgs, idx, item)
        {
            json_t*     jrole = json_object_get(item, "role");
            json_t*     jcontent = json_object_get(item, "content");
            const char* role = (jrole && json_is_string(jrole)) ? json_string_value(jrole) : "user";
            const char* content =
                (jcontent && json_is_string(jcontent)) ? json_string_value(jcontent) : "";

            if (strcmp(role, "system") == 0) {
                if (content[0] != '\0') {
                    size_t clen = strlen(content);
                    if (sys_buf == NULL) {
                        sys_buf = strdup(content);
                        sys_len = clen;
                    } else {
                        size_t nlen = sys_len + 2 + clen;
                        char*  nbuf = realloc(sys_buf, nlen + 1);
                        if (nbuf != NULL) {
                            sys_buf = nbuf;
                            memcpy(sys_buf + sys_len, "\n\n", 2);
                            memcpy(sys_buf + sys_len + 2, content, clen);
                            sys_len = nlen;
                            sys_buf[sys_len] = '\0';
                        }
                    }
                }
            } else {
                const char* ant_role = (strcmp(role, "assistant") == 0) ? "assistant" : "user";
                json_t*     m = json_object();
                json_object_set_new(m, "role", json_string(ant_role));
                json_object_set_new(m, "content", json_string(content));
                json_array_append_new(ant_msgs, m);
            }
        }
    }

    if (sys_buf != NULL) {
        json_object_set_new(out, "system", json_string(sys_buf));
        free(sys_buf);
    }
    json_object_set_new(out, "messages", ant_msgs);

    /* Max tokens: default 4096 if not specified */
    json_t* jmt = json_object_get(in_req, "max_tokens");
    if (jmt != NULL && json_is_integer(jmt)) {
        json_object_set(out, "max_tokens", jmt);
    } else {
        json_object_set_new(out, "max_tokens", json_integer(4096));
    }

    /* Temperature */
    json_t* jtemp = json_object_get(in_req, "temperature");
    if (jtemp != NULL && json_is_number(jtemp)) {
        json_object_set(out, "temperature", jtemp);
    }

    /* Top P */
    json_t* jtopp = json_object_get(in_req, "top_p");
    if (jtopp != NULL && json_is_number(jtopp)) {
        json_object_set(out, "top_p", jtopp);
    }

    /* Stop sequences */
    json_t* jstop = json_object_get(in_req, "stop");
    if (jstop != NULL) {
        if (json_is_string(jstop)) {
            json_t* arr = json_array();
            json_array_append(arr, jstop);
            json_object_set_new(out, "stop_sequences", arr);
        } else if (json_is_array(jstop)) {
            json_object_set(out, "stop_sequences", jstop);
        }
    }

    /* Stream */
    json_t* jstream = json_object_get(in_req, "stream");
    if (jstream != NULL && json_is_true(jstream)) {
        json_object_set_new(out, "stream", json_true());
    }

    /* Merge default_params if present */
    if (route->default_params_json[0] != '\0') {
        json_t* defaults = json_loads(route->default_params_json, 0, NULL);
        if (defaults != NULL) {
            json_t* merged = json_object();
            json_object_update(merged, defaults);
            json_object_update(merged, out);
            json_decref(out);
            json_decref(defaults);
            out = merged;
        }
    }

    char* packed = json_dumps(out, JSON_COMPACT);
    json_decref(out);
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

int
provider_anthropic_resp_to_openai(const char* anthropic_resp,
                                  const char* req_model,
                                  char**      out_openai,
                                  size_t*     out_openai_len,
                                  long*       out_ptok,
                                  long*       out_ctok)
{
    *out_ptok = 0;
    *out_ctok = 0;
    *out_openai = NULL;
    if (out_openai_len != NULL) {
        *out_openai_len = 0;
    }

    json_t* root = json_loads(anthropic_resp, 0, NULL);
    if (root == NULL) {
        return -1;
    }

    /* ID */
    const char* ant_id = "unknown";
    json_t*     jid = json_object_get(root, "id");
    if (jid != NULL && json_is_string(jid)) {
        ant_id = json_string_value(jid);
    }
    char oai_id[128];
    snprintf(oai_id, sizeof oai_id, "chatcmpl-%s", ant_id);

    /* Model */
    const char* model = req_model ? req_model : "claude";
    json_t*     jm = json_object_get(root, "model");
    if (jm != NULL && json_is_string(jm)) {
        model = json_string_value(jm);
    }

    /* Stop reason */
    const char* finish_reason = "stop";
    json_t*     jsr = json_object_get(root, "stop_reason");
    if (jsr != NULL && json_is_string(jsr)) {
        const char* sr = json_string_value(jsr);
        if (strcmp(sr, "max_tokens") == 0) {
            finish_reason = "length";
        }
    }

    /* Content text accumulation */
    char*   content_text = NULL;
    size_t  ct_len = 0;
    json_t* jcontent = json_object_get(root, "content");
    if (jcontent != NULL && json_is_array(jcontent)) {
        size_t  idx;
        json_t* block;
        json_array_foreach(jcontent, idx, block)
        {
            json_t* jtype = json_object_get(block, "type");
            if (jtype != NULL && json_is_string(jtype) &&
                strcmp(json_string_value(jtype), "text") == 0) {
                json_t* jt = json_object_get(block, "text");
                if (jt != NULL && json_is_string(jt)) {
                    const char* t = json_string_value(jt);
                    size_t      tlen = strlen(t);
                    char*       nbuf = realloc(content_text, ct_len + tlen + 1);
                    if (nbuf != NULL) {
                        content_text = nbuf;
                        memcpy(content_text + ct_len, t, tlen);
                        ct_len += tlen;
                        content_text[ct_len] = '\0';
                    }
                }
            }
        }
    }

    /* Usage */
    long    ptok = 0, ctok = 0;
    json_t* jusage = json_object_get(root, "usage");
    if (jusage != NULL && json_is_object(jusage)) {
        json_t* jin = json_object_get(jusage, "input_tokens");
        json_t* jout = json_object_get(jusage, "output_tokens");
        if (jin != NULL && json_is_integer(jin)) {
            ptok = json_integer_value(jin);
        }
        if (jout != NULL && json_is_integer(jout)) {
            ctok = json_integer_value(jout);
        }
    }
    *out_ptok = ptok;
    *out_ctok = ctok;

    /* Build OpenAI response */
    json_t* oai = json_object();
    json_object_set_new(oai, "id", json_string(oai_id));
    json_object_set_new(oai, "object", json_string("chat.completion"));
    json_object_set_new(oai, "created", json_integer((json_int_t)time(NULL)));
    json_object_set_new(oai, "model", json_string(model));

    json_t* choice = json_object();
    json_object_set_new(choice, "index", json_integer(0));
    json_t* msg = json_object();
    json_object_set_new(msg, "role", json_string("assistant"));
    json_object_set_new(msg, "content", json_string(content_text ? content_text : ""));
    json_object_set_new(choice, "message", msg);
    json_object_set_new(choice, "finish_reason", json_string(finish_reason));

    json_t* choices = json_array();
    json_array_append_new(choices, choice);
    json_object_set_new(oai, "choices", choices);

    json_t* usage_obj = json_object();
    json_object_set_new(usage_obj, "prompt_tokens", json_integer(ptok));
    json_object_set_new(usage_obj, "completion_tokens", json_integer(ctok));
    json_object_set_new(usage_obj, "total_tokens", json_integer(ptok + ctok));
    json_object_set_new(oai, "usage", usage_obj);

    free(content_text);
    json_decref(root);

    char* packed = json_dumps(oai, JSON_COMPACT);
    json_decref(oai);
    if (packed == NULL) {
        return -1;
    }
    *out_openai = packed;
    if (out_openai_len != NULL) {
        *out_openai_len = strlen(packed);
    }
    return 0;
}

void
anthropic_bridge_init(anthropic_bridge_t* b, aigate_response_ctx* rc)
{
    memset(b, 0, sizeof *b);
    b->rc = rc;
}

static void
bridge_send_chunk(anthropic_bridge_t* b, const char* chunk_str)
{
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
    if (b->rc->write != NULL) {
        if (b->rc->write(b->rc->impl, chunk_str, strlen(chunk_str), false) != 0) {
            b->aborted = true;
        }
    }
}

static void
bridge_process_line(anthropic_bridge_t* b, const char* line)
{
    if (strncmp(line, "event:", 6) == 0) {
        const char* ev = line + 6;
        while (*ev == ' ') {
            ev++;
        }
        snprintf(b->current_event, sizeof b->current_event, "%s", ev);
        return;
    }
    if (strncmp(line, "data:", 5) != 0) {
        if (line[0] == '\0') {
            b->current_event[0] = '\0';
        }
        return;
    }

    const char* d = line + 5;
    while (*d == ' ') {
        d++;
    }
    json_t* data = json_loads(d, 0, NULL);
    if (data == NULL) {
        return;
    }

    char id_buf[128];
    snprintf(id_buf, sizeof id_buf, "chatcmpl-%s", b->msg_id[0] ? b->msg_id : "claude");

    if (strcmp(b->current_event, "message_start") == 0) {
        json_t* jmsg = json_object_get(data, "message");
        if (jmsg != NULL && json_is_object(jmsg)) {
            json_t* jid = json_object_get(jmsg, "id");
            if (jid != NULL && json_is_string(jid)) {
                snprintf(b->msg_id, sizeof b->msg_id, "%s", json_string_value(jid));
                snprintf(id_buf, sizeof id_buf, "chatcmpl-%s", b->msg_id);
            }
            json_t* jmod = json_object_get(jmsg, "model");
            if (jmod != NULL && json_is_string(jmod)) {
                snprintf(b->model, sizeof b->model, "%s", json_string_value(jmod));
            }
            json_t* jusg = json_object_get(jmsg, "usage");
            if (jusg != NULL && json_is_object(jusg)) {
                json_t* jin = json_object_get(jusg, "input_tokens");
                if (jin != NULL && json_is_integer(jin)) {
                    b->input_tokens = json_integer_value(jin);
                }
            }
        }
        /* Emit initial role chunk */
        json_t* c0 = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{s:s,s:s},s:n}]}",
                               "id",
                               id_buf,
                               "object",
                               "chat.completion.chunk",
                               "model",
                               b->model,
                               "choices",
                               "index",
                               0,
                               "delta",
                               "role",
                               "assistant",
                               "content",
                               "",
                               "finish_reason");
        char*   p0 = json_dumps(c0, JSON_COMPACT);
        json_decref(c0);
        if (p0 != NULL) {
            char sse[8192];
            int  w = snprintf(sse, sizeof sse, "data: %s\n\n", p0);
            if (w >= (int)sizeof sse) {
                AIGATE_LOG_WARN("stream sse chunk truncated for model %s", b->model);
            }
            bridge_send_chunk(b, sse);
            free(p0);
        }
    } else if (strcmp(b->current_event, "content_block_delta") == 0) {
        json_t* jdel = json_object_get(data, "delta");
        if (jdel != NULL && json_is_object(jdel)) {
            json_t* jt = json_object_get(jdel, "text");
            if (jt != NULL && json_is_string(jt)) {
                const char* text = json_string_value(jt);
                json_t*     cd = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{s:s},s:n}]}",
                                           "id",
                                           id_buf,
                                           "object",
                                           "chat.completion.chunk",
                                           "model",
                                           b->model,
                                           "choices",
                                           "index",
                                           0,
                                           "delta",
                                           "content",
                                           text,
                                           "finish_reason");
                char*       pd = json_dumps(cd, JSON_COMPACT);
                json_decref(cd);
                if (pd != NULL) {
                    char sse[8192];
                    int  w = snprintf(sse, sizeof sse, "data: %s\n\n", pd);
                    if (w >= (int)sizeof sse) {
                        AIGATE_LOG_WARN("stream sse chunk truncated for model %s", b->model);
                    }
                    bridge_send_chunk(b, sse);
                    free(pd);
                }
            }
        }
    } else if (strcmp(b->current_event, "message_delta") == 0) {
        const char* finish_reason = "stop";
        json_t*     jdel = json_object_get(data, "delta");
        if (jdel != NULL && json_is_object(jdel)) {
            json_t* jsr = json_object_get(jdel, "stop_reason");
            if (jsr != NULL && json_is_string(jsr) &&
                strcmp(json_string_value(jsr), "max_tokens") == 0) {
                finish_reason = "length";
            }
        }
        json_t* jusg = json_object_get(data, "usage");
        if (jusg != NULL && json_is_object(jusg)) {
            json_t* jout = json_object_get(jusg, "output_tokens");
            if (jout != NULL && json_is_integer(jout)) {
                b->output_tokens = json_integer_value(jout);
            }
        }

        /* Emit finish_reason chunk */
        json_t* cf = json_pack("{s:s,s:s,s:s,s:[{s:i,s:{},s:s}]}",
                               "id",
                               id_buf,
                               "object",
                               "chat.completion.chunk",
                               "model",
                               b->model,
                               "choices",
                               "index",
                               0,
                               "delta",
                               "finish_reason",
                               finish_reason);
        char*   pf = json_dumps(cf, JSON_COMPACT);
        json_decref(cf);
        if (pf != NULL) {
            char sse[8192];
            int  w = snprintf(sse, sizeof sse, "data: %s\n\n", pf);
            if (w >= (int)sizeof sse) {
                AIGATE_LOG_WARN("stream sse chunk truncated for model %s", b->model);
            }
            bridge_send_chunk(b, sse);
            free(pf);
        }

        /* Emit usage chunk */
        json_t* cu = json_pack("{s:s,s:s,s:s,s:[],s:{s:i,s:i,s:i}}",
                               "id",
                               id_buf,
                               "object",
                               "chat.completion.chunk",
                               "model",
                               b->model,
                               "choices",
                               "usage",
                               "prompt_tokens",
                               (int)b->input_tokens,
                               "completion_tokens",
                               (int)b->output_tokens,
                               "total_tokens",
                               (int)(b->input_tokens + b->output_tokens));
        char*   pu = json_dumps(cu, JSON_COMPACT);
        json_decref(cu);
        if (pu != NULL) {
            char sse[8192];
            int  w = snprintf(sse, sizeof sse, "data: %s\n\n", pu);
            if (w >= (int)sizeof sse) {
                AIGATE_LOG_WARN("stream sse chunk truncated for model %s", b->model);
            }
            bridge_send_chunk(b, sse);
            free(pu);
        }
    } else if (strcmp(b->current_event, "message_stop") == 0) {
        bridge_send_chunk(b, "data: [DONE]\n\n");
        b->done_emitted = true;
    } else if (strcmp(b->current_event, "error") == 0) {
        const char* msg = "upstream error";
        json_t*     jerr = json_object_get(data, "error");
        if (jerr != NULL && json_is_object(jerr)) {
            json_t* jm = json_object_get(jerr, "message");
            if (jm != NULL && json_is_string(jm)) {
                msg = json_string_value(jm);
            }
        }
        char sse[8192];
        snprintf(
            sse,
            sizeof sse,
            "data: {\"error\":{\"message\":\"%s\",\"type\":\"upstream_error\",\"code\":502}}\n\n"
            "data: [DONE]\n\n",
            msg);
        bridge_send_chunk(b, sse);
        b->done_emitted = true;
    }

    json_decref(data);
}

int
anthropic_bridge_feed(anthropic_bridge_t* b, const void* chunk, size_t len)
{
    const char* p = chunk;
    const char* end = p + len;

    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (b->line_len + seg < sizeof b->line_buf) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                /* Strip trailing CR if present */
                if (b->line_len > 0 && b->line_buf[b->line_len - 1] == '\r') {
                    b->line_len--;
                }
                b->line_buf[b->line_len] = '\0';
                bridge_process_line(b, b->line_buf);
            } else {
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                b->model[0] ? b->model : "unknown");
            }
            b->line_len = 0;
            p = nl + 1;
        } else {
            size_t seg = (size_t)(end - p);
            if (b->line_len + seg < sizeof b->line_buf - 1) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                b->line_buf[b->line_len] = '\0';
            } else {
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                b->model[0] ? b->model : "unknown");
                b->line_len = 0;
            }
            p = end;
        }
    }
    return b->aborted ? -1 : 0;
}

int
anthropic_bridge_finish(anthropic_bridge_t* b)
{
    if (!b->done_emitted) {
        bridge_send_chunk(b, "data: [DONE]\n\n");
        b->done_emitted = true;
    }
    if (b->rc->write != NULL) {
        b->rc->write(b->rc->impl, "", 0, true);
    }
    return 0;
}

/* ------------------------------------------------------------ adapter impl */

static bool
adapter_anthropic_supports(const char* provider)
{
    return provider_anthropic_supports(provider) != 0;
}

static int
anthropic_build_chat(const model_rec_t* route,
                     const char*        in_body,
                     char*              url_out,
                     size_t             url_cap,
                     const char*        extra_headers[4][2],
                     int*               n_extra_headers,
                     char**             out_body,
                     size_t*            out_body_len)
{
    return provider_anthropic_build(
        route, in_body, url_out, url_cap, extra_headers, n_extra_headers, out_body, out_body_len);
}

static int
anthropic_parse_chat_response(const char* raw_body,
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
    if (out_cached_tok != NULL) {
        *out_cached_tok = 0;
    }
    *http_status = 200;
    return provider_anthropic_resp_to_openai(
        raw_body, model, out_body, out_len, out_ptok, out_ctok);
}

static stream_bridge_t*
anthropic_bridge_new(aigate_response_ctx* rc, const char* model)
{
    (void)model;
    anthropic_bridge_t* b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    anthropic_bridge_init(b, rc);
    return (stream_bridge_t*)b;
}

static int
anthropic_stream_bridge_feed(void* bridge, const void* chunk, size_t len)
{
    return anthropic_bridge_feed((anthropic_bridge_t*)bridge, chunk, len);
}

static int
anthropic_stream_bridge_finish(stream_bridge_t* b)
{
    return anthropic_bridge_finish((anthropic_bridge_t*)b);
}

static bool
anthropic_stream_bridge_headers_sent(stream_bridge_t* b)
{
    anthropic_bridge_t* ab = (anthropic_bridge_t*)b;
    return ab->headers_sent;
}

static void
anthropic_stream_bridge_get_tokens(stream_bridge_t* b,
                                   long*            out_ptok,
                                   long*            out_ctok,
                                   long*            out_cached_tok)
{
    anthropic_bridge_t* ab = (anthropic_bridge_t*)b;
    if (out_ptok) {
        *out_ptok = ab->input_tokens;
    }
    if (out_ctok) {
        *out_ctok = ab->output_tokens;
    }
    if (out_cached_tok) {
        *out_cached_tok = 0;
    }
}

static void
anthropic_stream_bridge_free(stream_bridge_t* b)
{
    free(b);
}

const provider_adapter_t g_provider_anthropic = {
    .name = "anthropic",
    .supports = adapter_anthropic_supports,
    .build_chat = anthropic_build_chat,
    .parse_chat_response = anthropic_parse_chat_response,
    .stream_bridge_new = anthropic_bridge_new,
    .stream_bridge_feed = anthropic_stream_bridge_feed,
    .stream_bridge_finish = anthropic_stream_bridge_finish,
    .stream_bridge_headers_sent = anthropic_stream_bridge_headers_sent,
    .stream_bridge_get_tokens = anthropic_stream_bridge_get_tokens,
    .stream_bridge_free = anthropic_stream_bridge_free,
    .build_embeddings = NULL,
    .parse_embeddings_response = NULL,
};
