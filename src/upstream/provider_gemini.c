/** @file provider_gemini.c
 *  @brief Google Gemini provider adapter (Plan 3, Tasks 3 & 4).
 *  SSE line buffer is 8192 bytes (P3-6): lines longer than that are
 *  truncated with a warn; usage extraction for the dropped part is lost.
 */
#include "provider_gemini.h"
#include "aigate_log.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/** @brief Infer image MIME type from URL path extension. Defaults to image/jpeg. */
static const char*
gemini_infer_mime_type(const char* url)
{
    if (!url) {
        return "image/jpeg";
    }
    const char* q = strchr(url, '?');
    size_t      path_len = q ? (size_t)(q - url) : strlen(url);
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".png", 4) == 0) {
        return "image/png";
    }
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".jpeg", 5) == 0) {
        return "image/jpeg";
    }
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".jpg", 4) == 0) {
        return "image/jpeg";
    }
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".webp", 5) == 0) {
        return "image/webp";
    }
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".gif", 4) == 0) {
        return "image/gif";
    }
    AIGATE_LOG_WARN("gemini_infer_mime_type: cannot infer from url, defaulting to image/jpeg");
    return "image/jpeg";
}

int
provider_gemini_supports(const char* provider)
{
    return provider != NULL && (strcmp(provider, "gemini") == 0 || strcmp(provider, "google") == 0);
}

/** @brief Adapter supports stub: delegates to provider_gemini_supports. */
static bool
adapter_gemini_supports(const char* provider)
{
    return provider_gemini_supports(provider) != 0;
}

/** @brief Look up function name for a tool_call_id by scanning backwards through messages.
 *  Falls back to tool_call_id itself if not found (logs WARN). */
static void
gemini_lookup_tool_name(json_t* msgs, const char* tool_call_id, char* out, size_t out_cap)
{
    if (!msgs || !json_is_array(msgs) || !tool_call_id) {
        snprintf(out, out_cap, "%s", tool_call_id ? tool_call_id : "unknown");
        return;
    }
    int n = (int)json_array_size(msgs);
    for (int i = n - 1; i >= 0; i--) {
        json_t* mi = json_array_get(msgs, i);
        json_t* jrole = json_object_get(mi, "role");
        if (!jrole || !json_is_string(jrole)) {
            continue;
        }
        if (strcmp(json_string_value(jrole), "assistant") != 0) {
            continue;
        }
        json_t* jtcs = json_object_get(mi, "tool_calls");
        if (!jtcs || !json_is_array(jtcs)) {
            continue;
        }
        size_t  ti;
        json_t* tc;
        json_array_foreach(jtcs, ti, tc)
        {
            json_t* jid = json_object_get(tc, "id");
            if (!jid || !json_is_string(jid)) {
                continue;
            }
            if (strcmp(json_string_value(jid), tool_call_id) == 0) {
                json_t* jfn = json_object_get(tc, "function");
                json_t* jname = jfn ? json_object_get(jfn, "name") : NULL;
                if (jname && json_is_string(jname)) {
                    snprintf(out, out_cap, "%s", json_string_value(jname));
                    return;
                }
            }
        }
    }
    AIGATE_LOG_WARN("gemini_lookup_tool_name: no match for tool_call_id=%s, using id as name",
                    tool_call_id);
    snprintf(out, out_cap, "%s", tool_call_id);
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
    bool    is_streaming = (jstream != NULL && json_is_true(jstream));

    /* Target URL */
    if (is_streaming) {
        snprintf(url_out,
                 url_cap,
                 "%s/v1beta/models/%s:streamGenerateContent?alt=sse",
                 base_ep,
                 route->name);
    } else {
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:generateContent", base_ep, route->name);
    }

    json_t* out_req = json_object();
    json_t* contents = json_array();

    /* Process messages */

    json_t* msgs = json_object_get(in_req, "messages");
    char    system_buf[8192];
    size_t  system_len = 0;
    system_buf[0] = '\0';

    if (msgs != NULL && json_is_array(msgs)) {
        size_t  idx;
        json_t* m;
        json_array_foreach(msgs, idx, m)
        {
            json_t*     jrole = json_object_get(m, "role");
            json_t*     jcontent = json_object_get(m, "content");
            const char* role = (jrole && json_is_string(jrole)) ? json_string_value(jrole) : "user";
            /* Plain text content (string) — for system/user/assistant fallback */
            const char* plain_content =
                (jcontent && json_is_string(jcontent)) ? json_string_value(jcontent) : "";

            if (strcmp(role, "system") == 0) {
                size_t clen = strlen(plain_content);
                if (clen > 0) {
                    if (system_len > 0 && system_len + 2 < sizeof(system_buf)) {
                        memcpy(system_buf + system_len, "\n\n", 2);
                        system_len += 2;
                    }
                    if (system_len + clen < sizeof(system_buf)) {
                        memcpy(system_buf + system_len, plain_content, clen);
                        system_len += clen;
                        system_buf[system_len] = '\0';
                    }
                }
            } else if (strcmp(role, "tool") == 0) {
                /* OpenAI tool result → Gemini functionResponse */
                json_t*     jtcid = json_object_get(m, "tool_call_id");
                const char* tcid = (jtcid && json_is_string(jtcid)) ? json_string_value(jtcid) : "";
                char        fname[128];
                gemini_lookup_tool_name(msgs, tcid, fname, sizeof fname);
                const char* result_str = plain_content;
                /* Try JSON parse; wrap as {output:...} if not */
                json_t* result_obj = json_loads(result_str, 0, NULL);
                if (result_obj == NULL) {
                    result_obj = json_object();
                    json_object_set_new(result_obj, "output", json_string(result_str));
                }
                json_t* fr = json_object();
                json_object_set_new(fr, "name", json_string(fname));
                json_object_set_new(fr, "response", result_obj);
                json_t* frp = json_object();
                json_object_set_new(frp, "functionResponse", fr);
                json_t* parts = json_array();
                json_array_append_new(parts, frp);
                json_t* entry = json_object();
                json_object_set_new(entry, "role", json_string("user"));
                json_object_set_new(entry, "parts", parts);
                json_array_append_new(contents, entry);
            } else {
                /* user or assistant message */
                const char* gemini_role = (strcmp(role, "assistant") == 0) ? "model" : "user";
                json_t*     jtool_calls = json_object_get(m, "tool_calls");
                if (strcmp(role, "assistant") == 0 && jtool_calls != NULL &&
                    json_is_array(jtool_calls) && json_array_size(jtool_calls) > 0) {
                    /* assistant + tool_calls → functionCall parts */
                    json_t* parts = json_array();
                    /* Optional text part */
                    if (plain_content && plain_content[0] != '\0') {
                        json_t* tp = json_object();
                        json_object_set_new(tp, "text", json_string(plain_content));
                        json_array_append_new(parts, tp);
                    }
                    size_t  ti;
                    json_t* tc;
                    json_array_foreach(jtool_calls, ti, tc)
                    {
                        json_t*     jfn = json_object_get(tc, "function");
                        const char* fn_name =
                            jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
                        const char* fn_args =
                            jfn ? json_string_value(json_object_get(jfn, "arguments")) : NULL;
                        if (!fn_name) {
                            continue;
                        }
                        json_t* args =
                            (fn_args && fn_args[0]) ? json_loads(fn_args, 0, NULL) : NULL;
                        if (!args) {
                            args = json_object();
                        }
                        json_t* fc = json_object();
                        json_object_set_new(fc, "name", json_string(fn_name));
                        json_object_set_new(fc, "args", args);
                        json_t* fcp = json_object();
                        json_object_set_new(fcp, "functionCall", fc);
                        json_array_append_new(parts, fcp);
                    }
                    json_t* entry = json_object();
                    json_object_set_new(entry, "role", json_string(gemini_role));
                    json_object_set_new(entry, "parts", parts);
                    json_array_append_new(contents, entry);
                } else {
                    /* user or assistant plain message / multimodal parts */
                    json_t* entry = json_object();
                    json_object_set_new(entry, "role", json_string(gemini_role));
                    json_t* parts = json_array();

                    if (jcontent && json_is_array(jcontent)) {
                        size_t  pi;
                        json_t* p;
                        json_array_foreach(jcontent, pi, p)
                        {
                            json_t*     jtype = json_object_get(p, "type");
                            const char* ptype =
                                (jtype && json_is_string(jtype)) ? json_string_value(jtype) : "";
                            if (strcmp(ptype, "text") == 0) {
                                json_t* jt = json_object_get(p, "text");
                                if (jt && json_is_string(jt)) {
                                    json_t* tp = json_object();
                                    json_object_set_new(
                                        tp, "text", json_string(json_string_value(jt)));
                                    json_array_append_new(parts, tp);
                                }
                            } else if (strcmp(ptype, "image_url") == 0) {
                                json_t* jiu = json_object_get(p, "image_url");
                                json_t* ju = jiu ? json_object_get(jiu, "url") : NULL;
                                if (ju && json_is_string(ju)) {
                                    const char* u = json_string_value(ju);
                                    const char* mime = gemini_infer_mime_type(u);
                                    json_t*     fd = json_object();
                                    json_object_set_new(fd, "fileUri", json_string(u));
                                    json_object_set_new(fd, "mimeType", json_string(mime));
                                    json_t* fdp = json_object();
                                    json_object_set_new(fdp, "fileData", fd);
                                    json_array_append_new(parts, fdp);
                                } else {
                                    AIGATE_LOG_WARN("image_url part missing url");
                                }
                            } else {
                                AIGATE_LOG_WARN("gemini: unsupported content part type '%s'",
                                                ptype);
                            }
                        }
                    } else {
                        json_t* part = json_object();
                        json_object_set_new(part, "text", json_string(plain_content));
                        json_array_append_new(parts, part);
                    }
                    json_object_set_new(entry, "parts", parts);
                    json_array_append_new(contents, entry);
                }
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

    /* tools → functionDeclarations */
    json_t* jtools = json_object_get(in_req, "tools");
    if (jtools != NULL && json_is_array(jtools) && json_array_size(jtools) > 0) {
        json_t* fn_decls = json_array();
        size_t  ti;
        json_t* tool;
        json_array_foreach(jtools, ti, tool)
        {
            json_t* jfn = json_object_get(tool, "function");
            if (!jfn) {
                continue;
            }
            json_t* fd = json_object();
            json_t* jname = json_object_get(jfn, "name");
            json_t* jdesc = json_object_get(jfn, "description");
            json_t* jparm = json_object_get(jfn, "parameters");
            if (jname) {
                json_object_set(fd, "name", jname);
            }
            if (jdesc) {
                json_object_set(fd, "description", jdesc);
            }
            if (jparm) {
                json_object_set_new(fd, "parameters", json_deep_copy(jparm));
            }
            json_array_append_new(fn_decls, fd);
        }
        json_t* tools_wrapper = json_object();
        json_object_set_new(tools_wrapper, "functionDeclarations", fn_decls);
        json_t* tools_arr = json_array();
        json_array_append_new(tools_arr, tools_wrapper);
        json_object_set_new(out_req, "tools", tools_arr);
    }

    /* tool_choice → toolConfig */
    json_t* jtc = json_object_get(in_req, "tool_choice");
    if (jtc != NULL) {
        const char* mode = "AUTO";
        json_t*     allowed = NULL;
        if (json_is_string(jtc)) {
            const char* s = json_string_value(jtc);
            if (strcmp(s, "required") == 0) {
                mode = "ANY";
            } else if (strcmp(s, "none") == 0) {
                mode = "NONE";
            }
            /* "auto" → "AUTO" (default) */
        } else if (json_is_object(jtc)) {
            json_t*     jfn = json_object_get(jtc, "function");
            const char* fname = jfn ? json_string_value(json_object_get(jfn, "name")) : NULL;
            mode = "ANY";
            if (fname) {
                allowed = json_array();
                json_array_append_new(allowed, json_string(fname));
            }
        }
        json_t* fcc = json_object();
        json_object_set_new(fcc, "mode", json_string(mode));
        if (allowed) {
            json_object_set_new(fcc, "allowedFunctionNames", allowed);
        }
        json_t* tool_cfg = json_object();
        json_object_set_new(tool_cfg, "functionCallingConfig", fcc);
        json_object_set_new(out_req, "toolConfig", tool_cfg);
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

/** @brief Map a native Gemini finish reason to an OpenAI finish_reason (STOP→stop, MAX_TOKENS→length, SAFETY/RECITATION→content_filter; unknown→stop).
 *  @return Borrowed static string; do not free. */
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
    if (out_ptok) {
        *out_ptok = 0;
    }
    if (out_ctok) {
        *out_ctok = 0;
    }
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
        json_t*     jmsg = json_object_get(jerr, "message");
        json_t*     jcode = json_object_get(jerr, "code");
        const char* msg =
            (jmsg && json_is_string(jmsg)) ? json_string_value(jmsg) : "unknown upstream error";
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

    /* Extract content from candidates[0].content.parts */
    char*       accum_text = NULL;
    size_t      accum_len = 0;
    json_t*     tool_calls_arr = json_array();
    const char* finish_reason = "stop";

    json_t* candidates = json_object_get(root, "candidates");
    if (candidates != NULL && json_is_array(candidates) && json_array_size(candidates) > 0) {
        json_t* c0 = json_array_get(candidates, 0);
        json_t* content = json_object_get(c0, "content");
        if (content != NULL && json_is_object(content)) {
            json_t* parts = json_object_get(content, "parts");
            if (parts != NULL && json_is_array(parts)) {
                size_t  pi;
                json_t* part;
                json_array_foreach(parts, pi, part)
                {
                    json_t* jtext = json_object_get(part, "text");
                    if (jtext && json_is_string(jtext)) {
                        const char* t = json_string_value(jtext);
                        size_t      tlen = strlen(t);
                        char*       nb = realloc(accum_text, accum_len + tlen + 1);
                        if (nb) {
                            accum_text = nb;
                            memcpy(accum_text + accum_len, t, tlen);
                            accum_len += tlen;
                            accum_text[accum_len] = '\0';
                        }
                    }
                    json_t* jfc = json_object_get(part, "functionCall");
                    if (jfc && json_is_object(jfc)) {
                        json_t*     jname = json_object_get(jfc, "name");
                        json_t*     jargs = json_object_get(jfc, "args");
                        const char* fname =
                            (jname && json_is_string(jname)) ? json_string_value(jname) : "";
                        char* args_str = jargs ? json_dumps(jargs, JSON_COMPACT) : strdup("{}");
                        char  call_id[64];
                        snprintf(call_id, sizeof call_id, "call_%s_%zu", fname, pi);
                        json_t* tc = json_object();
                        json_object_set_new(tc, "id", json_string(call_id));
                        json_object_set_new(tc, "type", json_string("function"));
                        json_t* fn = json_object();
                        json_object_set_new(fn, "name", json_string(fname));
                        json_object_set_new(
                            fn, "arguments", json_string(args_str ? args_str : "{}"));
                        free(args_str);
                        json_object_set_new(tc, "function", fn);
                        json_array_append_new(tool_calls_arr, tc);
                    }
                }
            }
        }
        json_t* jfinish = json_object_get(c0, "finishReason");
        if (jfinish != NULL && json_is_string(jfinish)) {
            finish_reason = map_gemini_finish_reason(json_string_value(jfinish));
        }
    }
    if (json_array_size(tool_calls_arr) > 0) {
        finish_reason = "tool_calls";
    }

    /* Usage metadata */
    long    ptok = 0, ctok = 0;
    json_t* usage = json_object_get(root, "usageMetadata");
    if (usage != NULL && json_is_object(usage)) {
        json_t* jp = json_object_get(usage, "promptTokenCount");
        json_t* jc = json_object_get(usage, "candidatesTokenCount");
        if (jp && json_is_integer(jp)) {
            ptok = json_integer_value(jp);
        }
        if (jc && json_is_integer(jc)) {
            ctok = json_integer_value(jc);
        }
    }
    if (out_ptok) {
        *out_ptok = ptok;
    }
    if (out_ctok) {
        *out_ctok = ctok;
    }

    /* Build OpenAI format response */
    json_t* oai = json_object();
    char    id[64];
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
    if (accum_text && accum_text[0] != '\0') {
        json_object_set_new(msg, "content", json_string(accum_text));
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
    json_array_append_new(choices, choice);
    json_object_set_new(oai, "choices", choices);

    json_t* usg = json_object();
    json_object_set_new(usg, "prompt_tokens", json_integer(ptok));
    json_object_set_new(usg, "completion_tokens", json_integer(ctok));
    json_object_set_new(usg, "total_tokens", json_integer(ptok + ctok));
    json_object_set_new(oai, "usage", usg);

    free(accum_text);
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

/** @brief Gemini→OpenAI streaming translation bridge state machine. */
typedef struct gemini_bridge {
    aigate_response_ctx* rc;                /**< Downstream response context (borrowed). */
    bool                 headers_sent;      /**< Downstream headers already sent. */
    bool                 aborted;           /**< Downstream aborted. */
    char                 line_buf[8192];    /**< SSE line buffer. */
    size_t               line_len;          /**< Line buffer bytes used. */
    char                 model[64];         /**< Model name. */
    char                 msg_id[64];        /**< Upstream message id. */
    char                 finish_reason[32]; /**< Upstream finishReason. */
    long                 prompt_tokens;     /**< Accumulated prompt tokens. */
    long                 completion_tokens; /**< Accumulated completion tokens. */
    bool                 done_emitted;      /**< [DONE] already emitted. */
} gemini_bridge_t; /**< Gemini streaming translation bridge type (see the gemini_bridge struct). */

/** @brief Create a Gemini→OpenAI streaming translation bridge. @return The bridge; NULL on OOM. */
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

/** @brief Emit one translated SSE chunk: sends the SSE header on the first packet; marks aborted if the downstream write fails.
 *  @return 0 on success; -1 on downstream write failure. */
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
    int wr = b->rc->write(b->rc->impl, str, strlen(str), false);
    if (wr != 0) {
        b->aborted = true;
    }
    return wr;
}

/** @brief Handle one Gemini SSE line: translates candidates/usageMetadata into OpenAI data lines and accumulates tokens. */
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
        if (jp && json_is_integer(jp)) {
            b->prompt_tokens = json_integer_value(jp);
        }
        if (jc && json_is_integer(jc)) {
            b->completion_tokens = json_integer_value(jc);
        }
    }

    /* Extract content and emit chunks for text / functionCall parts */
    json_t* candidates = json_object_get(root, "candidates");
    if (candidates != NULL && json_is_array(candidates) && json_array_size(candidates) > 0) {
        json_t* c0 = json_array_get(candidates, 0);
        json_t* content = json_object_get(c0, "content");
        if (content != NULL && json_is_object(content)) {
            json_t* parts = json_object_get(content, "parts");
            if (parts != NULL && json_is_array(parts)) {
                size_t  pi;
                json_t* part;
                json_array_foreach(parts, pi, part)
                {
                    /* Check for text delta */
                    json_t* jt = json_object_get(part, "text");
                    if (jt && json_is_string(jt)) {
                        const char* delta_text = json_string_value(jt);
                        if (delta_text[0] != '\0') {
                            json_t* chunk = json_object();
                            json_object_set_new(chunk, "id", json_string(b->msg_id));
                            json_object_set_new(
                                chunk, "object", json_string("chat.completion.chunk"));
                            json_object_set_new(
                                chunk, "created", json_integer((int64_t)time(NULL)));
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
                                char sse_line[8192];
                                int w = snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
                                if (w >= (int)sizeof sse_line) {
                                    AIGATE_LOG_WARN("stream sse chunk truncated for model %s",
                                                    b->model[0] ? b->model : "unknown");
                                }
                                free(packed);
                                gemini_bridge_send_chunk(b, sse_line);
                            }
                        }
                    }

                    /* Check for functionCall */
                    json_t* jfc = json_object_get(part, "functionCall");
                    if (jfc && json_is_object(jfc)) {
                        json_t*     jname = json_object_get(jfc, "name");
                        json_t*     jargs = json_object_get(jfc, "args");
                        const char* fname =
                            (jname && json_is_string(jname)) ? json_string_value(jname) : "";
                        char* args_str = jargs ? json_dumps(jargs, JSON_COMPACT) : strdup("{}");
                        char  call_id[64];
                        snprintf(call_id, sizeof call_id, "call_%s_%zu", fname, pi);

                        json_t* chunk = json_object();
                        json_object_set_new(chunk, "id", json_string(b->msg_id));
                        json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
                        json_object_set_new(chunk, "created", json_integer((int64_t)time(NULL)));
                        json_object_set_new(chunk, "model", json_string(b->model));

                        json_t* choices = json_array();
                        json_t* choice = json_object();
                        json_object_set_new(choice, "index", json_integer(0));

                        json_t* delta = json_object();
                        json_t* tc_arr = json_array();
                        json_t* tc = json_object();
                        json_object_set_new(tc, "index", json_integer((int)pi));
                        json_object_set_new(tc, "id", json_string(call_id));
                        json_object_set_new(tc, "type", json_string("function"));
                        json_t* fn = json_object();
                        json_object_set_new(fn, "name", json_string(fname));
                        json_object_set_new(
                            fn, "arguments", json_string(args_str ? args_str : "{}"));
                        free(args_str);
                        json_object_set_new(tc, "function", fn);
                        json_array_append_new(tc_arr, tc);

                        json_object_set_new(delta, "tool_calls", tc_arr);
                        json_object_set_new(choice, "delta", delta);
                        json_object_set_new(choice, "finish_reason", json_null());
                        json_array_append_new(choices, choice);
                        json_object_set_new(chunk, "choices", choices);

                        char* packed = json_dumps(chunk, JSON_COMPACT);
                        json_decref(chunk);
                        if (packed != NULL) {
                            char sse_line[8192];
                            int  w = snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
                            if (w >= (int)sizeof sse_line) {
                                AIGATE_LOG_WARN("stream sse chunk truncated for model %s",
                                                b->model[0] ? b->model : "unknown");
                            }
                            free(packed);
                            gemini_bridge_send_chunk(b, sse_line);
                        }
                        snprintf(b->finish_reason, sizeof b->finish_reason, "tool_calls");
                    }
                }
            }
        }
        json_t* jfinish = json_object_get(c0, "finishReason");
        if (jfinish != NULL && json_is_string(jfinish)) {
            if (strcmp(b->finish_reason, "tool_calls") != 0) {
                snprintf(b->finish_reason,
                         sizeof b->finish_reason,
                         "%s",
                         map_gemini_finish_reason(json_string_value(jfinish)));
            }
        }
    }

    json_decref(root);
}

/** @brief Streaming translation feed: translates Gemini SSE into OpenAI data lines line by line.
 *  @return 0 on success; -1 on downstream write failure. */
static int
gemini_stream_bridge_feed(void* bridge, const void* chunk, size_t len)
{
    gemini_bridge_t* b = bridge;
    const char*      p = chunk;
    const char*      end = p + len;

    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (b->line_len + seg < sizeof(b->line_buf)) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                while (b->line_len > 0 && (b->line_buf[b->line_len - 1] == '\r' ||
                                           b->line_buf[b->line_len - 1] == ' ')) {
                    b->line_len--;
                }
                b->line_buf[b->line_len] = '\0';
                gemini_bridge_process_line(b, b->line_buf);
                if (b->aborted) {
                    return -1;
                }
            } else {
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                b->model[0] ? b->model : "unknown");
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
                AIGATE_LOG_WARN("stream line truncated for model %s",
                                b->model[0] ? b->model : "unknown");
                b->line_len = 0;
            }
            p = end;
        }
    }
    return 0;
}

/** @brief Terminate the translation stream (emit [DONE] and fin). @return Downstream write result. */
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
        json_object_set_new(
            choice, "finish_reason", json_string(b->finish_reason[0] ? b->finish_reason : "stop"));
        json_array_append_new(choices, choice);
        json_object_set_new(chunk, "choices", choices);

        char* packed = json_dumps(chunk, JSON_COMPACT);
        json_decref(chunk);
        if (packed != NULL) {
            char sse_line[8192];
            int  w = snprintf(sse_line, sizeof sse_line, "data: %s\n\n", packed);
            if (w >= (int)sizeof sse_line) {
                AIGATE_LOG_WARN("stream sse chunk truncated for model %s",
                                b->model[0] ? b->model : "unknown");
            }
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

/** @brief Whether the translation bridge has sent headers. */
static bool
gemini_stream_bridge_headers_sent(stream_bridge_t* bridge)
{
    gemini_bridge_t* b = (gemini_bridge_t*)bridge;
    return b->headers_sent;
}

/** @brief Get the bridge's accumulated prompt/candidates/cached tokens (any out may be NULL). */
static void
gemini_stream_bridge_get_tokens(stream_bridge_t* bridge,
                                long*            out_ptok,
                                long*            out_ctok,
                                long*            out_cached_tok)
{
    gemini_bridge_t* b = (gemini_bridge_t*)bridge;
    if (out_ptok) {
        *out_ptok = b->prompt_tokens;
    }
    if (out_ctok) {
        *out_ctok = b->completion_tokens;
    }
    if (out_cached_tok) {
        *out_cached_tok = 0;
    }
}

/** @brief Free the translation bridge. */
static void
gemini_stream_bridge_free(stream_bridge_t* bridge)
{
    free(bridge);
}

/* ------------------------------------------------------------ adapter export */

/** @brief Adapter build_chat stub: translates an OpenAI request into a Gemini `generateContent` request (URL/body).
 *  @return 0 on success; -1 on URL overflow / allocation failure. */
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

/** @brief Adapter parse_chat_response stub: converts a non-streaming Gemini response to OpenAI format and extracts usage tokens.
 *  @return 0 on success; -1 on allocation failure. */
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
    if (out_cached_tok) {
        *out_cached_tok = 0;
    }
    *http_status = 200;
    return provider_gemini_resp_to_openai(raw_body, model, out_body, out_len, out_ptok, out_ctok);
}

int
provider_gemini_build_embeddings(const model_rec_t* route,
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
        return -1;
    }

    const char* model_name = route->name;
    json_t*     jm = json_object_get(in_req, "model");
    if (jm != NULL && json_is_string(jm)) {
        model_name = json_string_value(jm);
    }

    const char* url_model = model_name;
    if (strncmp(url_model, "models/", 7) == 0) {
        url_model += 7;
    }

    json_t* jdim = json_object_get(in_req, "dimensions");
    json_t* jinput = json_object_get(in_req, "input");

    json_t* gem_req = json_object();
    if (jinput != NULL && json_is_array(jinput)) {
        /* Batch mode: /v1beta/models/{model}:batchEmbedContents */
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:batchEmbedContents", base_ep, url_model);

        char model_path[256];
        if (strncmp(model_name, "models/", 7) == 0) {
            snprintf(model_path, sizeof model_path, "%s", model_name);
        } else {
            snprintf(model_path, sizeof model_path, "models/%s", model_name);
        }

        json_t* reqs = json_array();
        size_t  idx;
        json_t* item;
        json_array_foreach(jinput, idx, item)
        {
            const char* text = json_is_string(item) ? json_string_value(item) : "";
            json_t*     robj = json_object();
            json_object_set_new(robj, "model", json_string(model_path));

            json_t* content = json_object();
            json_t* parts = json_array();
            json_t* p = json_object();
            json_object_set_new(p, "text", json_string(text));
            json_array_append_new(parts, p);
            json_object_set_new(content, "parts", parts);
            json_object_set_new(robj, "content", content);

            if (jdim != NULL && json_is_integer(jdim)) {
                json_object_set_new(
                    robj, "outputDimensionality", json_integer(json_integer_value(jdim)));
            }
            json_array_append_new(reqs, robj);
        }
        json_object_set_new(gem_req, "requests", reqs);
    } else if (jinput != NULL && json_is_string(jinput)) {
        /* Single mode: /v1beta/models/{model}:embedContent */
        snprintf(url_out, url_cap, "%s/v1beta/models/%s:embedContent", base_ep, url_model);

        const char* text = json_string_value(jinput);
        json_t*     content = json_object();
        json_t*     parts = json_array();
        json_t*     p = json_object();
        json_object_set_new(p, "text", json_string(text ? text : ""));
        json_array_append_new(parts, p);
        json_object_set_new(content, "parts", parts);
        json_object_set_new(gem_req, "content", content);

        if (jdim != NULL && json_is_integer(jdim)) {
            json_object_set_new(
                gem_req, "outputDimensionality", json_integer(json_integer_value(jdim)));
        }
    } else {
        json_decref(gem_req);
        json_decref(in_req);
        return -1;
    }

    json_decref(in_req);

    char* packed = json_dumps(gem_req, JSON_COMPACT);
    json_decref(gem_req);
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
provider_gemini_parse_embeddings(const char* raw_body,
                                 size_t      raw_len,
                                 const char* model,
                                 int*        http_status,
                                 char**      out_body,
                                 size_t*     out_len,
                                 long*       out_ptok)
{
    (void)raw_len;
    if (out_ptok) {
        *out_ptok = 0;
    }
    *http_status = 200;

    if (raw_body == NULL || raw_body[0] == '\0') {
        return -1;
    }

    json_t* root = json_loads(raw_body, 0, NULL);
    if (root == NULL) {
        return -1;
    }

    /* Check for upstream error */
    json_t* jerr = json_object_get(root, "error");
    if (jerr != NULL && json_is_object(jerr)) {
        json_t*     jcode = json_object_get(jerr, "code");
        json_t*     jmsg = json_object_get(jerr, "message");
        int         err_code = json_is_integer(jcode) ? (int)json_integer_value(jcode) : 400;
        const char* err_msg = json_is_string(jmsg) ? json_string_value(jmsg) : "Gemini error";
        *http_status = err_code;

        json_t* err_resp = json_pack("{s:{s:s,s:s,s:i}}",
                                     "error",
                                     "message",
                                     err_msg,
                                     "type",
                                     "upstream_error",
                                     "code",
                                     err_code);
        json_decref(root);
        char* packed_err = json_dumps(err_resp, JSON_COMPACT);
        json_decref(err_resp);
        if (packed_err == NULL) {
            return -1;
        }
        *out_body = packed_err;
        *out_len = strlen(packed_err);
        return 0;
    }

    /* Extract usage token count */
    long    ptok = 0;
    json_t* um = json_object_get(root, "usageMetadata");
    if (um != NULL && json_is_object(um)) {
        json_t* pt = json_object_get(um, "promptTokenCount");
        if (json_is_integer(pt)) {
            ptok = json_integer_value(pt);
        }
    }

    json_t* data_arr = json_array();
    json_t* jemb = json_object_get(root, "embedding");
    json_t* jembs = json_object_get(root, "embeddings");

    if (jemb != NULL && json_is_object(jemb)) {
        json_t* jvals = json_object_get(jemb, "values");
        if (jvals != NULL && json_is_array(jvals)) {
            json_t* item = json_object();
            json_object_set_new(item, "object", json_string("embedding"));
            json_object_set_new(item, "index", json_integer(0));
            json_incref(jvals);
            json_object_set_new(item, "embedding", jvals);
            json_array_append_new(data_arr, item);
        }
        if (ptok == 0) {
            ptok = 1;
        }
    } else if (jembs != NULL && json_is_array(jembs)) {
        size_t  idx;
        json_t* entry;
        json_array_foreach(jembs, idx, entry)
        {
            json_t* jvals = json_object_get(entry, "values");
            if (jvals != NULL && json_is_array(jvals)) {
                json_t* item = json_object();
                json_object_set_new(item, "object", json_string("embedding"));
                json_object_set_new(item, "index", json_integer(idx));
                json_incref(jvals);
                json_object_set_new(item, "embedding", jvals);
                json_array_append_new(data_arr, item);
            }
        }
        if (ptok == 0) {
            ptok = (long)json_array_size(jembs);
        }
    } else {
        json_decref(data_arr);
        json_decref(root);
        return -1;
    }

    if (out_ptok) {
        *out_ptok = ptok;
    }

    json_t* resp = json_object();
    json_object_set_new(resp, "object", json_string("list"));
    json_object_set_new(resp, "data", data_arr);
    json_object_set_new(resp, "model", json_string(model ? model : "text-embedding-004"));

    json_t* usage = json_object();
    json_object_set_new(usage, "prompt_tokens", json_integer(ptok));
    json_object_set_new(usage, "total_tokens", json_integer(ptok));
    json_object_set_new(resp, "usage", usage);

    json_decref(root);

    char* packed = json_dumps(resp, JSON_COMPACT);
    json_decref(resp);
    if (packed == NULL) {
        return -1;
    }

    *out_body = packed;
    *out_len = strlen(packed);
    return 0;
}

/** @brief Gemini provider vtable instance (see the provider_adapter vtable). */
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
    .build_embeddings = provider_gemini_build_embeddings,
    .parse_embeddings_response = provider_gemini_parse_embeddings,
};

int
gemini_sniff_usage_json(const char* json_str, long* out_ptok, long* out_ctok, long* out_cached)
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
    json_t* um = json_object_get(root, "usageMetadata");
    if (um != NULL && json_is_object(um)) {
        json_t* jp = json_object_get(um, "promptTokenCount");
        json_t* jc = json_object_get(um, "candidatesTokenCount");
        json_t* jcached = json_object_get(um, "cachedContentTokenCount");
        if (out_ptok && jp && json_is_integer(jp)) {
            *out_ptok = (long)json_integer_value(jp);
        }
        if (out_ctok && jc && json_is_integer(jc)) {
            *out_ctok = (long)json_integer_value(jc);
        }
        if (out_cached && jcached && json_is_integer(jcached)) {
            *out_cached = (long)json_integer_value(jcached);
        }
    }
    json_decref(root);
    return 0;
}

void
gemini_sniffer_init(gemini_sniffer_t* s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

/** @brief Sniffer line handler: parses usageMetadata lines to accumulate tokens. */
static void
gemini_sniffer_process_line(gemini_sniffer_t* s, const char* line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (strncmp(line, "data: ", 6) == 0) {
        const char* payload = line + 6;
        if (strcmp(payload, "[DONE]") == 0) {
            return;
        }
        if (strstr(payload, "usageMetadata") != NULL) {
            json_t* root = json_loads(payload, 0, NULL);
            if (root != NULL) {
                json_t* um = json_object_get(root, "usageMetadata");
                if (um != NULL && json_is_object(um)) {
                    json_t* jp = json_object_get(um, "promptTokenCount");
                    json_t* jc = json_object_get(um, "candidatesTokenCount");
                    json_t* jcached = json_object_get(um, "cachedContentTokenCount");
                    if (jp && json_is_integer(jp)) {
                        s->prompt_tokens = (long)json_integer_value(jp);
                    }
                    if (jc && json_is_integer(jc)) {
                        s->candidates_tokens = (long)json_integer_value(jc);
                    }
                    if (jcached && json_is_integer(jcached)) {
                        s->cached_tokens = (long)json_integer_value(jcached);
                    }
                }
                json_decref(root);
            }
        }
    }
}

int
gemini_sniffer_feed(gemini_sniffer_t* s, const void* chunk, size_t len)
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
                gemini_sniffer_process_line(s, s->line_buf);
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
gemini_sniffer_get_tokens(const gemini_sniffer_t* s,
                          long*                   out_ptok,
                          long*                   out_ctok,
                          long*                   out_cached)
{
    if (s == NULL) {
        return;
    }
    if (out_ptok) {
        *out_ptok = s->prompt_tokens;
    }
    if (out_ctok) {
        *out_ctok = s->candidates_tokens;
    }
    if (out_cached) {
        *out_cached = s->cached_tokens;
    }
}
