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

/** @brief Extract plain text from a content field that may be a string or content-part array.
 *  Caller must free the returned string. */
static char*
ant_extract_text(json_t* jcontent)
{
    if (jcontent == NULL) {
        return strdup("");
    }
    if (json_is_string(jcontent)) {
        return strdup(json_string_value(jcontent));
    }
    if (!json_is_array(jcontent)) {
        return strdup("");
    }
    /* Content-part array: concatenate all type=="text" parts */
    char*   buf = NULL;
    size_t  buf_len = 0;
    size_t  idx;
    json_t* part;
    json_array_foreach(jcontent, idx, part)
    {
        json_t* jtype = json_object_get(part, "type");
        if (!jtype || !json_is_string(jtype)) {
            continue;
        }
        if (strcmp(json_string_value(jtype), "text") == 0) {
            json_t* jt = json_object_get(part, "text");
            if (jt && json_is_string(jt)) {
                const char* t = json_string_value(jt);
                size_t      tlen = strlen(t);
                char*       nb = realloc(buf, buf_len + tlen + 1);
                if (nb) {
                    buf = nb;
                    memcpy(buf + buf_len, t, tlen);
                    buf_len += tlen;
                    buf[buf_len] = '\0';
                }
            }
        } else if (strcmp(json_string_value(jtype), "image_url") == 0) {
            AIGATE_LOG_WARN(
                "image_url content part ignored (vision not supported by Anthropic adapter)");
        }
    }
    return buf ? buf : strdup("");
}

/** @brief Build Anthropic content: returns json_string (if plain string input) or json_array (if content parts).
 *  Caller takes ownership of returned json_t*. */
static json_t*
ant_build_content_array(json_t* jcontent)
{
    if (jcontent == NULL) {
        return json_string("");
    }
    if (json_is_string(jcontent)) {
        return json_string(json_string_value(jcontent));
    }
    if (!json_is_array(jcontent)) {
        return json_string("");
    }
    json_t* arr = json_array();
    size_t  idx;
    json_t* part;
    json_array_foreach(jcontent, idx, part)
    {
        json_t* jtype = json_object_get(part, "type");
        if (!jtype || !json_is_string(jtype)) {
            continue;
        }
        const char* type_str = json_string_value(jtype);
        if (strcmp(type_str, "text") == 0) {
            json_t* jt = json_object_get(part, "text");
            if (jt && json_is_string(jt)) {
                json_t* tb = json_object();
                json_object_set_new(tb, "type", json_string("text"));
                json_object_set_new(tb, "text", json_string(json_string_value(jt)));
                json_array_append_new(arr, tb);
            }
        } else if (strcmp(type_str, "image_url") == 0) {
            json_t* jiu = json_object_get(part, "image_url");
            json_t* ju = jiu ? json_object_get(jiu, "url") : NULL;
            if (ju && json_is_string(ju)) {
                json_t* img = json_object();
                json_object_set_new(img, "type", json_string("image"));
                json_t* src = json_object();
                json_object_set_new(src, "type", json_string("url"));
                json_object_set_new(src, "url", json_string(json_string_value(ju)));
                json_object_set_new(img, "source", src);
                json_array_append_new(arr, img);
            } else {
                AIGATE_LOG_WARN("image_url content part missing url");
            }
        } else {
            AIGATE_LOG_WARN("unsupported content part type '%s'", type_str);
        }
    }
    return arr;
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

            if (strcmp(role, "system") == 0) {
                char*  txt = ant_extract_text(jcontent);
                size_t clen = strlen(txt);
                if (clen > 0) {
                    if (sys_buf == NULL) {
                        sys_buf = strdup(txt);
                        sys_len = clen;
                    } else {
                        size_t nlen = sys_len + 2 + clen;
                        char*  nbuf = realloc(sys_buf, nlen + 1);
                        if (nbuf != NULL) {
                            sys_buf = nbuf;
                            memcpy(sys_buf + sys_len, "\n\n", 2);
                            memcpy(sys_buf + sys_len + 2, txt, clen);
                            sys_len = nlen;
                            sys_buf[sys_len] = '\0';
                        }
                    }
                }
                free(txt);

            } else if (strcmp(role, "tool") == 0) {
                /* OpenAI tool result → Anthropic tool_result content block */
                json_t*     jtcid = json_object_get(item, "tool_call_id");
                const char* tcid = (jtcid && json_is_string(jtcid)) ? json_string_value(jtcid) : "";
                char*       txt = ant_extract_text(jcontent);
                json_t*     tr = json_object();
                json_object_set_new(tr, "type", json_string("tool_result"));
                json_object_set_new(tr, "tool_use_id", json_string(tcid));
                json_object_set_new(tr, "content", json_string(txt));
                free(txt);
                json_t* tr_content = json_array();
                json_array_append_new(tr_content, tr);
                json_t* m = json_object();
                json_object_set_new(m, "role", json_string("user"));
                json_object_set_new(m, "content", tr_content);
                json_array_append_new(ant_msgs, m);

            } else {
                /* user or assistant message */
                const char* ant_role = (strcmp(role, "assistant") == 0) ? "assistant" : "user";
                json_t*     jtool_calls = json_object_get(item, "tool_calls");
                if (strcmp(ant_role, "assistant") == 0 && jtool_calls != NULL &&
                    json_is_array(jtool_calls) && json_array_size(jtool_calls) > 0) {
                    /* assistant message with tool_calls → Anthropic content array */
                    json_t* ant_content = json_array();
                    char*   txt = ant_extract_text(jcontent);
                    if (txt && txt[0] != '\0') {
                        json_t* tb = json_object();
                        json_object_set_new(tb, "type", json_string("text"));
                        json_object_set_new(tb, "text", json_string(txt));
                        json_array_append_new(ant_content, tb);
                    }
                    free(txt);
                    size_t  ti;
                    json_t* tc;
                    json_array_foreach(jtool_calls, ti, tc)
                    {
                        json_t*     jfn = json_object_get(tc, "function");
                        json_t*     jtcid = json_object_get(tc, "id");
                        const char* tc_id =
                            (jtcid && json_is_string(jtcid)) ? json_string_value(jtcid) : "";
                        const char* fn_name =
                            jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
                        const char* fn_args =
                            jfn ? json_string_value(json_object_get(jfn, "arguments")) : NULL;
                        if (!fn_name) {
                            continue;
                        }
                        json_t* input =
                            (fn_args && fn_args[0]) ? json_loads(fn_args, 0, NULL) : NULL;
                        if (!input) {
                            input = json_object();
                        }
                        json_t* tub = json_object();
                        json_object_set_new(tub, "type", json_string("tool_use"));
                        json_object_set_new(tub, "id", json_string(tc_id));
                        json_object_set_new(tub, "name", json_string(fn_name));
                        json_object_set_new(tub, "input", input);
                        json_array_append_new(ant_content, tub);
                    }
                    json_t* m = json_object();
                    json_object_set_new(m, "role", json_string("assistant"));
                    json_object_set_new(m, "content", ant_content);
                    json_array_append_new(ant_msgs, m);
                } else {
                    /* plain text or multimodal user/assistant message */
                    json_t* ant_content = ant_build_content_array(jcontent);
                    json_t* m = json_object();
                    json_object_set_new(m, "role", json_string(ant_role));
                    json_object_set_new(m, "content", ant_content ? ant_content : json_string(""));
                    json_array_append_new(ant_msgs, m);
                }
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

    /* tools → Anthropic tools with input_schema */
    json_t* jtools = json_object_get(in_req, "tools");
    if (jtools != NULL && json_is_array(jtools) && json_array_size(jtools) > 0) {
        json_t* ant_tools = json_array();
        size_t  ti;
        json_t* tool;
        json_array_foreach(jtools, ti, tool)
        {
            json_t* jfn = json_object_get(tool, "function");
            if (!jfn) {
                continue;
            }
            json_t* at = json_object();
            json_t* jname = json_object_get(jfn, "name");
            json_t* jdesc = json_object_get(jfn, "description");
            json_t* jparm = json_object_get(jfn, "parameters");
            if (jname) {
                json_object_set(at, "name", jname);
            }
            if (jdesc) {
                json_object_set(at, "description", jdesc);
            }
            if (jparm) {
                json_object_set_new(at, "input_schema", json_deep_copy(jparm));
            }
            json_array_append_new(ant_tools, at);
        }
        json_object_set_new(out, "tools", ant_tools);
    }

    /* tool_choice → Anthropic tool_choice */
    json_t* jtc = json_object_get(in_req, "tool_choice");
    if (jtc != NULL) {
        json_t* ant_tc = NULL;
        if (json_is_string(jtc)) {
            const char* s = json_string_value(jtc);
            if (strcmp(s, "auto") == 0) {
                ant_tc = json_pack("{ss}", "type", "auto");
            } else if (strcmp(s, "required") == 0) {
                ant_tc = json_pack("{ss}", "type", "any");
            } else if (strcmp(s, "none") == 0) {
                ant_tc = json_pack("{ss}", "type", "none");
            }
        } else if (json_is_object(jtc)) {
            json_t*     jfn = json_object_get(jtc, "function");
            const char* fname = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
            if (fname) {
                ant_tc = json_pack("{ssss}", "type", "tool", "name", fname);
            }
        }
        if (ant_tc) {
            json_object_set_new(out, "tool_choice", ant_tc);
        }
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

    /* Stop reason → finish_reason */
    const char* finish_reason = "stop";
    json_t*     jsr = json_object_get(root, "stop_reason");
    if (jsr != NULL && json_is_string(jsr)) {
        const char* sr = json_string_value(jsr);
        if (strcmp(sr, "max_tokens") == 0) {
            finish_reason = "length";
        } else if (strcmp(sr, "tool_use") == 0) {
            finish_reason = "tool_calls";
        }
    }

    /* Scan content array: collect text and tool_use blocks */
    char*   content_text = NULL;
    size_t  ct_len = 0;
    json_t* tool_calls_arr = json_array();

    json_t* jcontent = json_object_get(root, "content");
    if (jcontent != NULL && json_is_array(jcontent)) {
        size_t  idx;
        json_t* block;
        json_array_foreach(jcontent, idx, block)
        {
            json_t* jtype = json_object_get(block, "type");
            if (jtype == NULL || !json_is_string(jtype)) {
                continue;
            }
            const char* btype = json_string_value(jtype);

            if (strcmp(btype, "text") == 0) {
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
            } else if (strcmp(btype, "tool_use") == 0) {
                json_t*     jtid = json_object_get(block, "id");
                json_t*     jtname = json_object_get(block, "name");
                json_t*     jtinput = json_object_get(block, "input");
                const char* tid = (jtid && json_is_string(jtid)) ? json_string_value(jtid) : "";
                const char* tname =
                    (jtname && json_is_string(jtname)) ? json_string_value(jtname) : "";
                char*   args_str = jtinput ? json_dumps(jtinput, JSON_COMPACT) : strdup("{}");
                json_t* tc = json_object();
                json_object_set_new(tc, "id", json_string(tid));
                json_object_set_new(tc, "type", json_string("function"));
                json_t* fn = json_object();
                json_object_set_new(fn, "name", json_string(tname));
                json_object_set_new(fn, "arguments", json_string(args_str ? args_str : "{}"));
                free(args_str);
                json_object_set_new(tc, "function", fn);
                json_array_append_new(tool_calls_arr, tc);
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
    /* content: text if present, null if pure tool_calls */
    if (content_text && content_text[0] != '\0') {
        json_object_set_new(msg, "content", json_string(content_text));
    } else {
        json_object_set_new(msg, "content", json_null());
    }
    if (json_array_size(tool_calls_arr) > 0) {
        json_object_set_new(msg, "tool_calls", tool_calls_arr);
    } else {
        json_decref(tool_calls_arr);
    }
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

/** @brief Emit one translated chunk: sends the SSE header on the first packet; marks aborted if the downstream write fails. */
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

/** @brief Handle one Anthropic SSE line: `event:` lines record the current event type (message_start/usage, etc.). */
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
    } else if (strcmp(b->current_event, "content_block_start") == 0) {
        /* Detect tool_use blocks; reset accumulation state */
        json_t* jcb = json_object_get(data, "content_block");
        if (jcb != NULL && json_is_object(jcb)) {
            json_t* jt = json_object_get(jcb, "type");
            if (jt && json_is_string(jt) && strcmp(json_string_value(jt), "tool_use") == 0) {
                b->in_tool_use = true;
                json_t* jid = json_object_get(jcb, "id");
                json_t* jname = json_object_get(jcb, "name");
                snprintf(b->tool_id,
                         sizeof b->tool_id,
                         "%s",
                         (jid && json_is_string(jid)) ? json_string_value(jid) : "");
                snprintf(b->tool_name,
                         sizeof b->tool_name,
                         "%s",
                         (jname && json_is_string(jname)) ? json_string_value(jname) : "");
                free(b->tool_args_buf);
                b->tool_args_buf = NULL;
                b->tool_args_len = 0;
            } else {
                b->in_tool_use = false;
            }
        }
    } else if (strcmp(b->current_event, "content_block_delta") == 0) {
        json_t* jdel = json_object_get(data, "delta");
        if (jdel != NULL && json_is_object(jdel)) {
            json_t*     jtype = json_object_get(jdel, "type");
            const char* dtype = (jtype && json_is_string(jtype)) ? json_string_value(jtype) : "";
            if (strcmp(dtype, "text_delta") == 0 || strcmp(dtype, "") == 0) {
                /* text delta (may be keyed "text" or "value") */
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
            } else if (strcmp(dtype, "input_json_delta") == 0 && b->in_tool_use) {
                /* Tool argument partial JSON — accumulate */
                json_t* jpj = json_object_get(jdel, "partial_json");
                if (jpj && json_is_string(jpj)) {
                    const char* pj = json_string_value(jpj);
                    size_t      pjlen = strlen(pj);
                    if (b->tool_args_len + pjlen < 65536) {
                        char* nb = realloc(b->tool_args_buf, b->tool_args_len + pjlen + 1);
                        if (nb) {
                            b->tool_args_buf = nb;
                            memcpy(b->tool_args_buf + b->tool_args_len, pj, pjlen);
                            b->tool_args_len += pjlen;
                            b->tool_args_buf[b->tool_args_len] = '\0';
                        }
                    } else {
                        AIGATE_LOG_WARN("tool_args_buf >64KB for model %s, truncating", b->model);
                    }
                }
            }
        }
    } else if (strcmp(b->current_event, "content_block_stop") == 0) {
        /* Emit tool_calls chunk when a tool_use block completes */
        if (b->in_tool_use && b->tool_id[0] != '\0') {
            const char* args = b->tool_args_buf ? b->tool_args_buf : "{}";
            json_t*     chunk = json_object();
            json_object_set_new(chunk, "id", json_string(id_buf));
            json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
            json_object_set_new(chunk, "model", json_string(b->model));
            json_t* choices = json_array();
            json_t* choice = json_object();
            json_object_set_new(choice, "index", json_integer(0));
            json_t* delta = json_object();
            json_t* tc_arr = json_array();
            json_t* tc = json_object();
            json_object_set_new(tc, "index", json_integer(b->tool_index));
            json_object_set_new(tc, "id", json_string(b->tool_id));
            json_object_set_new(tc, "type", json_string("function"));
            json_t* fn = json_object();
            json_object_set_new(fn, "name", json_string(b->tool_name));
            json_object_set_new(fn, "arguments", json_string(args));
            json_object_set_new(tc, "function", fn);
            json_array_append_new(tc_arr, tc);
            json_object_set_new(delta, "tool_calls", tc_arr);
            json_object_set_new(choice, "delta", delta);
            json_object_set_new(choice, "finish_reason", json_null());
            json_array_append_new(choices, choice);
            json_object_set_new(chunk, "choices", choices);
            char* packed = json_dumps(chunk, JSON_COMPACT);
            json_decref(chunk);
            if (packed) {
                char sse[8192];
                int  w = snprintf(sse, sizeof sse, "data: %s\n\n", packed);
                if (w >= (int)sizeof sse) {
                    AIGATE_LOG_WARN("stream sse chunk truncated for model %s", b->model);
                }
                bridge_send_chunk(b, sse);
                free(packed);
            }
            b->in_tool_use = false;
            b->tool_index++;
        }
    } else if (strcmp(b->current_event, "message_delta") == 0) {
        const char* finish_reason = "stop";
        json_t*     jdel = json_object_get(data, "delta");
        if (jdel != NULL && json_is_object(jdel)) {
            json_t* jsr = json_object_get(jdel, "stop_reason");
            if (jsr != NULL && json_is_string(jsr)) {
                const char* sr = json_string_value(jsr);
                if (strcmp(sr, "max_tokens") == 0) {
                    finish_reason = "length";
                } else if (strcmp(sr, "tool_use") == 0) {
                    finish_reason = "tool_calls";
                }
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

/** @brief Adapter supports stub: delegates to provider_anthropic_supports. */
static bool
adapter_anthropic_supports(const char* provider)
{
    return provider_anthropic_supports(provider) != 0;
}

/** @brief Adapter build_chat stub: translates an OpenAI request into an Anthropic `/messages` request (URL/headers/body).
 *  @return 0 on success; -1 on URL overflow / allocation failure. */
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

/** @brief Adapter parse_chat_response stub: converts a non-streaming Anthropic response to OpenAI format and extracts usage tokens.
 *  @return 0 on success; -1 on allocation failure. */
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

/** @brief Create an Anthropic→OpenAI streaming translation bridge. @return The bridge; NULL on OOM. */
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

/** @brief Streaming translation feed: converts Anthropic SSE events into OpenAI data lines and accumulates usage.
 *  @return 0 on success; -1 on downstream write failure. */
static int
anthropic_stream_bridge_feed(void* bridge, const void* chunk, size_t len)
{
    return anthropic_bridge_feed((anthropic_bridge_t*)bridge, chunk, len);
}

/** @brief Terminate the translation stream (emit [DONE] and fin). @return Downstream write result. */
static int
anthropic_stream_bridge_finish(stream_bridge_t* b)
{
    return anthropic_bridge_finish((anthropic_bridge_t*)b);
}

/** @brief Whether the translation bridge has sent headers. */
static bool
anthropic_stream_bridge_headers_sent(stream_bridge_t* b)
{
    anthropic_bridge_t* ab = (anthropic_bridge_t*)b;
    return ab->headers_sent;
}

/** @brief Get the bridge's accumulated input/output/cached tokens (any out may be NULL). */
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

/** @brief Free the translation bridge. */
static void
anthropic_stream_bridge_free(stream_bridge_t* b)
{
    anthropic_bridge_t* ab = (anthropic_bridge_t*)b;
    free(ab->tool_args_buf);
    free(b);
}

/** @brief Anthropic provider vtable instance (see the provider_adapter vtable). */
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

int
anthropic_sniff_usage_json(const char* json_str, long* out_ptok, long* out_ctok, long* out_cached)
{
    if (out_ptok) {
        *out_ptok = 0;
    }
    if (out_ctok) {
        *out_ctok = 0;
    }
    if (out_cached) {
        *out_cached = 0;
    }
    if (json_str == NULL || json_str[0] == '\0') {
        return -1;
    }
    json_error_t err;
    json_t*      root = json_loads(json_str, 0, &err);
    if (root == NULL) {
        return -1;
    }
    json_t* usage = json_object_get(root, "usage");
    if (usage != NULL && json_is_object(usage)) {
        json_t* ji = json_object_get(usage, "input_tokens");
        json_t* jo = json_object_get(usage, "output_tokens");
        json_t* jcr = json_object_get(usage, "cache_read_input_tokens");
        if (out_ptok && ji && json_is_integer(ji)) {
            *out_ptok = (long)json_integer_value(ji);
        }
        if (out_ctok && jo && json_is_integer(jo)) {
            *out_ctok = (long)json_integer_value(jo);
        }
        long cached = 0;
        if (jcr && json_is_integer(jcr)) {
            cached = (long)json_integer_value(jcr);
        }
        if (out_cached) {
            *out_cached = cached;
        }
    }
    json_decref(root);
    return 0;
}

void
anthropic_sniffer_init(anthropic_sniffer_t* s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

/** @brief Sniffer line handler: parses message_start/usage event lines to accumulate tokens. */
static void
anthropic_sniffer_process_line(anthropic_sniffer_t* s, const char* line)
{
    if (line == NULL || line[0] == '\0') {
        s->current_event[0] = '\0';
        return;
    }
    if (strncmp(line, "event: ", 7) == 0) {
        snprintf(s->current_event, sizeof(s->current_event), "%s", line + 7);
        return;
    }
    if (strncmp(line, "data: ", 6) == 0) {
        const char* payload = line + 6;
        if (strcmp(payload, "[DONE]") == 0) {
            return;
        }
        if (strcmp(s->current_event, "message_start") == 0) {
            json_t* root = json_loads(payload, 0, NULL);
            if (root != NULL) {
                json_t* msg = json_object_get(root, "message");
                if (msg != NULL) {
                    json_t* usage = json_object_get(msg, "usage");
                    if (usage != NULL) {
                        json_t* ji = json_object_get(usage, "input_tokens");
                        json_t* jcr = json_object_get(usage, "cache_read_input_tokens");
                        if (ji && json_is_integer(ji)) {
                            s->input_tokens = (long)json_integer_value(ji);
                        }
                        if (jcr && json_is_integer(jcr)) {
                            s->cached_tokens = (long)json_integer_value(jcr);
                        }
                    }
                }
                json_decref(root);
            }
        } else if (strcmp(s->current_event, "message_delta") == 0) {
            json_t* root = json_loads(payload, 0, NULL);
            if (root != NULL) {
                json_t* usage = json_object_get(root, "usage");
                if (usage != NULL) {
                    json_t* jo = json_object_get(usage, "output_tokens");
                    if (jo && json_is_integer(jo)) {
                        s->output_tokens = (long)json_integer_value(jo);
                    }
                }
                json_decref(root);
            }
        }
    }
}

int
anthropic_sniffer_feed(anthropic_sniffer_t* s, const void* chunk, size_t len)
{
    if (s == NULL || chunk == NULL || len == 0) {
        return 0;
    }
    const char* p = chunk;
    const char* end = p + len;

    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (s->line_len + seg < sizeof(s->line_buf)) {
                memcpy(s->line_buf + s->line_len, p, seg);
                s->line_len += seg;
                if (s->line_len > 0 && s->line_buf[s->line_len - 1] == '\r') {
                    s->line_len--;
                }
                s->line_buf[s->line_len] = '\0';
                anthropic_sniffer_process_line(s, s->line_buf);
            }
            s->line_len = 0;
            p = nl + 1;
        } else {
            size_t seg = (size_t)(end - p);
            if (s->line_len + seg < sizeof(s->line_buf) - 1) {
                memcpy(s->line_buf + s->line_len, p, seg);
                s->line_len += seg;
                s->line_buf[s->line_len] = '\0';
            } else {
                s->line_len = 0;
            }
            p = end;
        }
    }
    return 0;
}

void
anthropic_sniffer_get_tokens(const anthropic_sniffer_t* s,
                             long*                      out_ptok,
                             long*                      out_ctok,
                             long*                      out_cached)
{
    if (s == NULL) {
        return;
    }
    if (out_ptok) {
        *out_ptok = s->input_tokens;
    }
    if (out_ctok) {
        *out_ctok = s->output_tokens;
    }
    if (out_cached) {
        *out_cached = s->cached_tokens;
    }
}
