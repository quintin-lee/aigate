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

    /* Check reasoning / thinking configuration */
    reasoning_config_t rcfg;
    parse_reasoning_config(req, route, &rcfg);

    /* Check if request has reasoning_effort */
    json_t* jeff = json_object_get(req, "reasoning_effort");
    bool has_reasoning_effort = (jeff != NULL && json_is_string(jeff));
    bool is_o_series = (strstr(route->name, "o1") != NULL ||
                        strstr(route->name, "o3") != NULL ||
                        (route->endpoint[0] != '\0' && strstr(route->endpoint, "o1") != NULL));

    if (has_reasoning_effort) {
        const char* eff = json_string_value(jeff);
        if (strcmp(eff, "low") != 0 && strcmp(eff, "medium") != 0 &&
            strcmp(eff, "high") != 0 && strcmp(eff, "none") != 0) {
            json_object_set_new(req, "reasoning_effort", json_string("medium"));
        }
    }

    /* If degraded (rcfg was disabled or budget == 0), strip reasoning_effort */
    if (!rcfg.enabled || rcfg.budget_tokens == 0) {
        if (has_reasoning_effort) {
            json_object_del(req, "reasoning_effort");
            has_reasoning_effort = false;
        }
    }

    /* If targeting o-series model or reasoning_effort is active, map max_tokens -> max_completion_tokens */
    if (is_o_series || has_reasoning_effort) {
        json_t* jmt = json_object_get(req, "max_tokens");
        json_t* jmct = json_object_get(req, "max_completion_tokens");
        if (jmt != NULL && jmct == NULL) {
            json_object_set(req, "max_completion_tokens", jmt);
            json_object_del(req, "max_tokens");
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

int
provider_openai_resp_format_tag_emulation(const char* raw_body,
                                          size_t      raw_len,
                                          const char* model,
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

    if (raw_body == NULL || raw_len == 0) {
        return -1;
    }

    json_error_t err;
    json_t* root = json_loadb(raw_body, raw_len, 0, &err);
    if (root == NULL) {
        return -1;
    }

    /* Extract tokens from usage */
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

    /* Process choices[0].message for reasoning tag emulation */
    json_t* choices = json_object_get(root, "choices");
    if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
        json_t* c0 = json_array_get(choices, 0);
        if (c0 != NULL && json_is_object(c0)) {
            json_t* msg = json_object_get(c0, "message");
            if (msg != NULL && json_is_object(msg)) {
                json_t* jrc = json_object_get(msg, "reasoning_content");
                if (jrc == NULL) {
                    jrc = json_object_get(msg, "thought");
                }
                if (jrc != NULL && json_is_string(jrc)) {
                    const char* rtext = json_string_value(jrc);
                    if (rtext[0] != '\0') {
                        json_t* jcontent = json_object_get(msg, "content");
                        const char* ctext = (jcontent && json_is_string(jcontent)) ? json_string_value(jcontent) : "";
                        size_t rlen = strlen(rtext);
                        size_t clen = strlen(ctext);
                        size_t new_len = rlen + clen + 32;
                        char* new_content = malloc(new_len);
                        if (new_content != NULL) {
                            if (clen > 0) {
                                snprintf(new_content, new_len, "<think>\n%s\n</think>\n\n%s", rtext, ctext);
                            } else {
                                snprintf(new_content, new_len, "<think>\n%s\n</think>", rtext);
                            }
                            json_object_set_new(msg, "content", json_string(new_content));
                            free(new_content);
                        }
                    }
                }
            }
        }
    }

    char* packed = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (packed == NULL) {
        return -1;
    }
    *out_body = packed;
    if (out_len != NULL) {
        *out_len = strlen(packed);
    }
    return 0;
}

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

static void
openai_bridge_emit_sse(openai_bridge_t* b, const char* sse_str)
{
    if (b->rc != NULL && b->rc->write != NULL && sse_str != NULL && sse_str[0] != '\0') {
        b->rc->write(b->rc->impl, sse_str, strlen(sse_str), false);
    }
}

static void
openai_bridge_emit_delta(openai_bridge_t* b, const char* field, const char* text, const char* finish_reason)
{
    char id_buf[256];
    snprintf(id_buf, sizeof(id_buf), "chatcmpl-%s", b->model[0] ? b->model : "openai");
    json_t* chunk = json_object();
    json_object_set_new(chunk, "id", json_string(id_buf));
    json_object_set_new(chunk, "object", json_string("chat.completion.chunk"));
    json_object_set_new(chunk, "model", json_string(b->model));
    json_t* choices = json_array();
    json_t* c0 = json_object();
    json_object_set_new(c0, "index", json_integer(0));
    json_t* delta = json_object();
    if (field != NULL && text != NULL) {
        json_object_set_new(delta, field, json_string(text));
    }
    json_object_set_new(c0, "delta", delta);
    if (finish_reason != NULL) {
        json_object_set_new(c0, "finish_reason", json_string(finish_reason));
    } else {
        json_object_set_new(c0, "finish_reason", json_null());
    }
    json_array_append_new(choices, c0);
    json_object_set_new(chunk, "choices", choices);

    char* packed = json_dumps(chunk, JSON_COMPACT);
    json_decref(chunk);
    if (packed != NULL) {
        char sse[8192];
        snprintf(sse, sizeof(sse), "data: %s\n\n", packed);
        openai_bridge_emit_sse(b, sse);
        free(packed);
    }
}

/** @brief Parse `"usage"`-bearing SSE lines in the passthrough stream, accumulating prompt/completion/cached/reasoning tokens and handling tag emulation. */
static void
openai_stream_process_line(openai_bridge_t* acc, const char* line)
{
    const char* d = strstr(line, "data:");
    if (d == NULL) {
        return;
    }

    /* 1. Extract usage if present */
    const char* u = strstr(d, "\"usage\"");
    if (u != NULL) {
        const char* jstart = strchr(d, '{');
        if (jstart != NULL) {
            json_t* root = json_loads(jstart, 0, NULL);
            if (root != NULL) {
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
        }
    }

    const char* dp = d + 5;
    while (*dp == ' ') {
        dp++;
    }

    /* 2. Tag emulation mode */
    if (acc->tag_emulation) {
        if (strcmp(dp, "[DONE]") == 0) {
            if (acc->tag_opened && !acc->tag_closed) {
                openai_bridge_emit_delta(acc, "content", "\n</think>\n\n", NULL);
                acc->tag_closed = true;
            }
            openai_bridge_emit_sse(acc, "data: [DONE]\n\n");
            return;
        }

        const char* jstart = strchr(dp, '{');
        if (jstart != NULL) {
            json_t* root = json_loads(jstart, 0, NULL);
            if (root != NULL) {
                json_t* choices = json_object_get(root, "choices");
                if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
                    json_t* c0 = json_array_get(choices, 0);
                    if (c0 != NULL && json_is_object(c0)) {
                        json_t* delta = json_object_get(c0, "delta");
                        if (delta != NULL && json_is_object(delta)) {
                            json_t* jrc = json_object_get(delta, "reasoning_content");
                            if (jrc == NULL) {
                                jrc = json_object_get(delta, "thought");
                            }
                            if (jrc != NULL && json_is_string(jrc)) {
                                const char* rtext = json_string_value(jrc);
                                if (!acc->tag_opened) {
                                    openai_bridge_emit_delta(acc, "content", "<think>\n", NULL);
                                    acc->tag_opened = true;
                                }
                                openai_bridge_emit_delta(acc, "content", rtext, NULL);
                            }

                            json_t* jc = json_object_get(delta, "content");
                            if (jc != NULL && json_is_string(jc)) {
                                const char* ctext = json_string_value(jc);
                                if (acc->tag_opened && !acc->tag_closed) {
                                    openai_bridge_emit_delta(acc, "content", "\n</think>\n\n", NULL);
                                    acc->tag_closed = true;
                                }
                                openai_bridge_emit_delta(acc, "content", ctext, NULL);
                            }
                        }

                        json_t* jfr = json_object_get(c0, "finish_reason");
                        if (jfr != NULL && !json_is_null(jfr)) {
                            if (acc->tag_opened && !acc->tag_closed) {
                                openai_bridge_emit_delta(acc, "content", "\n</think>\n\n", NULL);
                                acc->tag_closed = true;
                            }
                            const char* fr_str = json_is_string(jfr) ? json_string_value(jfr) : "stop";
                            openai_bridge_emit_delta(acc, NULL, NULL, fr_str);
                        }
                    }
                }
                json_decref(root);
            }
        }
        return;
    }

    /* 3. Raw mode (tag_emulation == false): normalize thought -> reasoning_content */
    if (strcmp(dp, "[DONE]") == 0) {
        openai_bridge_emit_sse(acc, "data: [DONE]\n\n");
        return;
    }

    const char* jstart = strchr(dp, '{');
    if (jstart != NULL) {
        json_t* root = json_loads(jstart, 0, NULL);
        if (root != NULL) {
            bool modified = false;
            json_t* choices = json_object_get(root, "choices");
            if (choices != NULL && json_is_array(choices) && json_array_size(choices) > 0) {
                json_t* c0 = json_array_get(choices, 0);
                if (c0 != NULL && json_is_object(c0)) {
                    json_t* delta = json_object_get(c0, "delta");
                    if (delta != NULL && json_is_object(delta)) {
                        json_t* jth = json_object_get(delta, "thought");
                        json_t* jrc = json_object_get(delta, "reasoning_content");
                        if (jth != NULL && jrc == NULL) {
                            json_object_set(delta, "reasoning_content", jth);
                            json_object_del(delta, "thought");
                            modified = true;
                        }
                    }
                }
            }
            if (modified) {
                char* packed = json_dumps(root, JSON_COMPACT);
                if (packed != NULL) {
                    char sse[8192];
                    snprintf(sse, sizeof(sse), "data: %s\n\n", packed);
                    openai_bridge_emit_sse(acc, sse);
                    free(packed);
                }
            } else {
                char sse[8192];
                snprintf(sse, sizeof(sse), "%s\n\n", line);
                openai_bridge_emit_sse(acc, sse);
            }
            json_decref(root);
            return;
        }
    }

    char sse[8192];
    snprintf(sse, sizeof(sse), "%s\n\n", line);
    openai_bridge_emit_sse(acc, sse);
}

/** @brief Passthrough stream feed: sends the SSE header on the first packet, handles tag emulation or normalizes deltas per line.
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

    const char* p = chunk;
    const char* end = p + len;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (acc->line_len + seg < sizeof(acc->line_buf)) {
                memcpy(acc->line_buf + acc->line_len, p, seg);
                acc->line_len += seg;
                if (acc->line_len > 0 && acc->line_buf[acc->line_len - 1] == '\r') {
                    acc->line_len--;
                }
                acc->line_buf[acc->line_len] = '\0';
                if (acc->line_len > 0) {
                    openai_stream_process_line(acc, acc->line_buf);
                }
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

/** @brief Terminate the stream (closes <think> tag if needed, empty write as fin marker). @return Downstream write result. */
static int
openai_bridge_finish(stream_bridge_t* b)
{
    openai_bridge_t* acc = (openai_bridge_t*)b;
    if (acc->tag_emulation && acc->tag_opened && !acc->tag_closed) {
        openai_bridge_emit_delta(acc, "content", "\n</think>\n\n", NULL);
        acc->tag_closed = true;
    }
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

typedef struct openai_to_anthropic_bridge {
    aigate_response_ctx* rc;
    bool                 headers_sent;
    bool                 aborted;
    char                 line_buf[8192];
    size_t               line_len;
    char                 msg_id[64];
    char                 model[64];
    bool                 message_start_sent;
    bool                 in_thinking;
    bool                 thinking_started;
    bool                 thinking_closed;
    bool                 text_started;
    bool                 text_closed;
    long                 input_tokens;
    long                 output_tokens;
    long                 cached_tokens;
    char                 finish_reason[64];
    bool                 done_emitted;
} openai_to_anthropic_bridge_t;

static void
oai_ant_bridge_send(openai_to_anthropic_bridge_t* b, const char* sse_str)
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
    if (b->rc->write != NULL && sse_str != NULL && sse_str[0] != '\0') {
        if (b->rc->write(b->rc->impl, sse_str, strlen(sse_str), false) != 0) {
            b->aborted = true;
        }
    }
}

static void
oai_ant_process_line(openai_to_anthropic_bridge_t* b, const char* line)
{
    if (strncmp(line, "data:", 5) != 0) {
        return;
    }
    const char* d = line + 5;
    while (*d == ' ') {
        d++;
    }
    if (strncmp(d, "[DONE]", 6) == 0) {
        return;
    }

    json_error_t err;
    json_t* root = json_loads(d, 0, &err);
    if (root == NULL) {
        return;
    }

    /* Extract id and model if not yet set */
    if (b->msg_id[0] == '\0') {
        json_t* jid = json_object_get(root, "id");
        if (jid && json_is_string(jid)) {
            snprintf(b->msg_id, sizeof b->msg_id, "%s", json_string_value(jid));
        }
    }
    if (b->model[0] == '\0') {
        json_t* jm = json_object_get(root, "model");
        if (jm && json_is_string(jm)) {
            snprintf(b->model, sizeof b->model, "%s", json_string_value(jm));
        }
    }

    /* Emit message_start if not yet sent */
    if (!b->message_start_sent) {
        char msg_id_clean[128];
        if (strncmp(b->msg_id, "chatcmpl-", 9) == 0) {
            snprintf(msg_id_clean, sizeof msg_id_clean, "msg_%s", b->msg_id + 9);
        } else if (b->msg_id[0] != '\0') {
            snprintf(msg_id_clean, sizeof msg_id_clean, "msg_%s", b->msg_id);
        } else {
            snprintf(msg_id_clean, sizeof msg_id_clean, "msg_unknown");
        }
        json_t* ms = json_pack("{s:s,s:{s:s,s:s,s:s,s:s,s:[],s:n,s:n,s:{s:i,s:i}}}",
                               "type", "message_start",
                               "message",
                               "id", msg_id_clean,
                               "type", "message",
                               "role", "assistant",
                               "model", b->model[0] ? b->model : "unknown",
                               "content",
                               "stop_reason",
                               "stop_sequence",
                               "usage",
                               "input_tokens", (int)b->input_tokens,
                               "output_tokens", 0);
        char* pms = json_dumps(ms, JSON_COMPACT);
        json_decref(ms);
        if (pms) {
            char sse[2048];
            snprintf(sse, sizeof sse, "event: message_start\ndata: %s\n\n", pms);
            oai_ant_bridge_send(b, sse);
            free(pms);
        }
        b->message_start_sent = true;
    }

    /* Usage check */
    json_t* jusage = json_object_get(root, "usage");
    if (jusage && json_is_object(jusage)) {
        json_t* jin = json_object_get(jusage, "prompt_tokens");
        if (jin == NULL) jin = json_object_get(jusage, "input_tokens");
        if (jin && json_is_integer(jin)) {
            b->input_tokens = json_integer_value(jin);
        }
        json_t* jout = json_object_get(jusage, "completion_tokens");
        if (jout == NULL) jout = json_object_get(jusage, "output_tokens");
        if (jout && json_is_integer(jout)) {
            b->output_tokens = json_integer_value(jout);
        }
    }

    /* Choices processing */
    json_t* choices = json_object_get(root, "choices");
    if (choices && json_is_array(choices) && json_array_size(choices) > 0) {
        json_t* c0 = json_array_get(choices, 0);
        if (c0 && json_is_object(c0)) {
            json_t* jfr = json_object_get(c0, "finish_reason");
            if (jfr && json_is_string(jfr)) {
                const char* fr = json_string_value(jfr);
                if (strcmp(fr, "length") == 0) {
                    snprintf(b->finish_reason, sizeof b->finish_reason, "max_tokens");
                } else if (strcmp(fr, "tool_calls") == 0) {
                    snprintf(b->finish_reason, sizeof b->finish_reason, "tool_use");
                } else if (strcmp(fr, "stop") == 0) {
                    snprintf(b->finish_reason, sizeof b->finish_reason, "end_turn");
                }
            }

            json_t* jdel = json_object_get(c0, "delta");
            if (jdel && json_is_object(jdel)) {
                /* 1. Reasoning / thinking content */
                json_t* jrc = json_object_get(jdel, "reasoning_content");
                if (jrc && json_is_string(jrc)) {
                    const char* rtext = json_string_value(jrc);
                    if (!b->thinking_started) {
                        json_t* bs = json_pack("{s:s,s:i,s:{s:s,s:s}}",
                                               "type", "content_block_start",
                                               "index", 0,
                                               "content_block",
                                               "type", "thinking",
                                               "thinking", "");
                        char* pbs = json_dumps(bs, JSON_COMPACT);
                        json_decref(bs);
                        if (pbs) {
                            char sse[1024];
                            snprintf(sse, sizeof sse, "event: content_block_start\ndata: %s\n\n", pbs);
                            oai_ant_bridge_send(b, sse);
                            free(pbs);
                        }
                        b->thinking_started = true;
                        b->in_thinking = true;
                    }
                    json_t* bd = json_pack("{s:s,s:i,s:{s:s,s:s}}",
                                           "type", "content_block_delta",
                                           "index", 0,
                                           "delta",
                                           "type", "thinking_delta",
                                           "thinking", rtext);
                    char* pbd = json_dumps(bd, JSON_COMPACT);
                    json_decref(bd);
                    if (pbd) {
                        char sse[8192];
                        snprintf(sse, sizeof sse, "event: content_block_delta\ndata: %s\n\n", pbd);
                        oai_ant_bridge_send(b, sse);
                        free(pbd);
                    }
                }

                /* 2. Text content */
                json_t* jc = json_object_get(jdel, "content");
                if (jc && json_is_string(jc)) {
                    const char* ctext = json_string_value(jc);
                    if (b->in_thinking && !b->thinking_closed) {
                        json_t* bstop = json_pack("{s:s,s:i}",
                                                  "type", "content_block_stop",
                                                  "index", 0);
                        char* pbstop = json_dumps(bstop, JSON_COMPACT);
                        json_decref(bstop);
                        if (pbstop) {
                            char sse[512];
                            snprintf(sse, sizeof sse, "event: content_block_stop\ndata: %s\n\n", pbstop);
                            oai_ant_bridge_send(b, sse);
                            free(pbstop);
                        }
                        b->thinking_closed = true;
                        b->in_thinking = false;
                    }
                    int text_idx = b->thinking_started ? 1 : 0;
                    if (!b->text_started) {
                        json_t* bs = json_pack("{s:s,s:i,s:{s:s,s:s}}",
                                               "type", "content_block_start",
                                               "index", text_idx,
                                               "content_block",
                                               "type", "text",
                                               "text", "");
                        char* pbs = json_dumps(bs, JSON_COMPACT);
                        json_decref(bs);
                        if (pbs) {
                            char sse[1024];
                            snprintf(sse, sizeof sse, "event: content_block_start\ndata: %s\n\n", pbs);
                            oai_ant_bridge_send(b, sse);
                            free(pbs);
                        }
                        b->text_started = true;
                    }
                    json_t* bd = json_pack("{s:s,s:i,s:{s:s,s:s}}",
                                           "type", "content_block_delta",
                                           "index", text_idx,
                                           "delta",
                                           "type", "text_delta",
                                           "text", ctext);
                    char* pbd = json_dumps(bd, JSON_COMPACT);
                    json_decref(bd);
                    if (pbd) {
                        char sse[8192];
                        snprintf(sse, sizeof sse, "event: content_block_delta\ndata: %s\n\n", pbd);
                        oai_ant_bridge_send(b, sse);
                        free(pbd);
                    }
                }
            }
        }
    }

    json_decref(root);
}

stream_bridge_t*
provider_openai_to_anthropic_bridge_new(aigate_response_ctx* rc, const char* model)
{
    openai_to_anthropic_bridge_t* b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->rc = rc;
    if (model) {
        snprintf(b->model, sizeof b->model, "%s", model);
    }
    return (stream_bridge_t*)b;
}

int
provider_openai_to_anthropic_bridge_feed(stream_bridge_t* bridge, const void* chunk, size_t len)
{
    openai_to_anthropic_bridge_t* b = (openai_to_anthropic_bridge_t*)bridge;
    if (b == NULL) {
        return -1;
    }

    const char* p = chunk;
    const char* end = p + len;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (nl != NULL) {
            size_t seg = (size_t)(nl - p);
            if (b->line_len + seg < sizeof(b->line_buf)) {
                memcpy(b->line_buf + b->line_len, p, seg);
                b->line_len += seg;
                if (b->line_len > 0 && b->line_buf[b->line_len - 1] == '\r') {
                    b->line_len--;
                }
                b->line_buf[b->line_len] = '\0';
                oai_ant_process_line(b, b->line_buf);
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
    return b->aborted ? -1 : 0;
}

int
provider_openai_to_anthropic_bridge_finish(stream_bridge_t* bridge)
{
    openai_to_anthropic_bridge_t* b = (openai_to_anthropic_bridge_t*)bridge;
    if (b == NULL) {
        return 0;
    }
    if (!b->done_emitted) {
        if (b->in_thinking && !b->thinking_closed) {
            json_t* bstop = json_pack("{s:s,s:i}",
                                      "type", "content_block_stop",
                                      "index", 0);
            char* pbstop = json_dumps(bstop, JSON_COMPACT);
            json_decref(bstop);
            if (pbstop) {
                char sse[512];
                snprintf(sse, sizeof sse, "event: content_block_stop\ndata: %s\n\n", pbstop);
                oai_ant_bridge_send(b, sse);
                free(pbstop);
            }
            b->thinking_closed = true;
            b->in_thinking = false;
        }
        if (b->text_started && !b->text_closed) {
            int text_idx = b->thinking_started ? 1 : 0;
            json_t* bstop = json_pack("{s:s,s:i}",
                                      "type", "content_block_stop",
                                      "index", text_idx);
            char* pbstop = json_dumps(bstop, JSON_COMPACT);
            json_decref(bstop);
            if (pbstop) {
                char sse[512];
                snprintf(sse, sizeof sse, "event: content_block_stop\ndata: %s\n\n", pbstop);
                oai_ant_bridge_send(b, sse);
                free(pbstop);
            }
            b->text_closed = true;
        }

        const char* sr = b->finish_reason[0] ? b->finish_reason : "end_turn";
        json_t* md = json_pack("{s:s,s:{s:s,s:n},s:{s:i}}",
                               "type", "message_delta",
                               "delta",
                               "stop_reason", sr,
                               "stop_sequence",
                               "usage",
                               "output_tokens", (int)b->output_tokens);
        char* pmd = json_dumps(md, JSON_COMPACT);
        json_decref(md);
        if (pmd) {
            char sse[1024];
            snprintf(sse, sizeof sse, "event: message_delta\ndata: %s\n\n", pmd);
            oai_ant_bridge_send(b, sse);
            free(pmd);
        }

        oai_ant_bridge_send(b, "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
        b->done_emitted = true;
    }
    if (b->rc->write != NULL) {
        b->rc->write(b->rc->impl, "", 0, true);
    }
    return 0;
}

void
provider_openai_to_anthropic_bridge_free(stream_bridge_t* bridge)
{
    if (bridge != NULL) {
        free(bridge);
    }
}

void
provider_openai_to_anthropic_bridge_get_tokens(stream_bridge_t* bridge,
                                               long*            out_ptok,
                                               long*            out_ctok,
                                               long*            out_cached_tok)
{
    openai_to_anthropic_bridge_t* b = (openai_to_anthropic_bridge_t*)bridge;
    if (b == NULL) {
        return;
    }
    if (out_ptok) *out_ptok = b->input_tokens;
    if (out_ctok) *out_ctok = b->output_tokens;
    if (out_cached_tok) *out_cached_tok = b->cached_tokens;
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
