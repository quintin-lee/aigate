#include "prompt_template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

prompt_inject_mode_t
prompt_mode_from_str(const char* str)
{
    if (str == NULL || str[0] == '\0') {
        return PROMPT_MODE_PREPEND;
    }
    if (strcasecmp(str, "append") == 0) {
        return PROMPT_MODE_APPEND;
    }
    if (strcasecmp(str, "override") == 0) {
        return PROMPT_MODE_OVERRIDE;
    }
    return PROMPT_MODE_PREPEND;
}

const char*
prompt_mode_to_str(prompt_inject_mode_t mode)
{
    switch (mode) {
    case PROMPT_MODE_APPEND:
        return "append";
    case PROMPT_MODE_OVERRIDE:
        return "override";
    case PROMPT_MODE_PREPEND:
    default:
        return "prepend";
    }
}

char*
prompt_template_expand_vars(const char* tmpl, const char* model, const char* key_name)
{
    if (tmpl == NULL) {
        return NULL;
    }

    time_t    now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);

    char date_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &gmt);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &gmt);

    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)now);

    const char* m_val = (model != NULL && model[0] != '\0') ? model : "";
    const char* k_val = (key_name != NULL && key_name[0] != '\0') ? key_name : "";

    size_t cap = strlen(tmpl) + 256;
    char*  out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    size_t      out_len = 0;
    const char* p = tmpl;

    while (*p != '\0') {
        if (p[0] == '$' && p[1] == '{') {
            const char* end = strchr(p + 2, '}');
            if (end != NULL) {
                size_t      var_len = (size_t)(end - (p + 2));
                const char* rep = NULL;
                size_t      rep_len = 0;

                if (var_len == 4 && strncmp(p + 2, "date", 4) == 0) {
                    rep = date_buf;
                    rep_len = strlen(date_buf);
                } else if (var_len == 4 && strncmp(p + 2, "time", 4) == 0) {
                    rep = time_buf;
                    rep_len = strlen(time_buf);
                } else if (var_len == 9 && strncmp(p + 2, "timestamp", 9) == 0) {
                    rep = ts_buf;
                    rep_len = strlen(ts_buf);
                } else if (var_len == 5 && strncmp(p + 2, "model", 5) == 0) {
                    rep = m_val;
                    rep_len = strlen(m_val);
                } else if (var_len == 8 && strncmp(p + 2, "key_name", 8) == 0) {
                    rep = k_val;
                    rep_len = strlen(k_val);
                }

                if (rep != NULL) {
                    while (out_len + rep_len + 1 >= cap) {
                        cap *= 2;
                        char* n = realloc(out, cap);
                        if (n == NULL) {
                            free(out);
                            return NULL;
                        }
                        out = n;
                    }
                    memcpy(out + out_len, rep, rep_len);
                    out_len += rep_len;
                    p = end + 1;
                    continue;
                }
            }
        }

        if (out_len + 2 >= cap) {
            cap *= 2;
            char* n = realloc(out, cap);
            if (n == NULL) {
                free(out);
                return NULL;
            }
            out = n;
        }
        out[out_len++] = *p++;
    }

    out[out_len] = '\0';
    return out;
}

int
prompt_template_apply(const prompt_template_t* tmpl,
                      const char*              model,
                      const char*              key_name,
                      json_t*                  jbody,
                      char**                   out_modified_json,
                      size_t*                  out_len)
{
    if (tmpl == NULL || jbody == NULL || !json_is_object(jbody)) {
        return -1;
    }
    if (tmpl->system_template == NULL || tmpl->system_template[0] == '\0') {
        return -1;
    }
    if (out_modified_json == NULL) {
        return -1;
    }

    char* expanded = prompt_template_expand_vars(tmpl->system_template, model, key_name);
    if (expanded == NULL) {
        return -1;
    }

    json_t* messages = json_object_get(jbody, "messages");
    if (messages == NULL || !json_is_array(messages)) {
        free(expanded);
        return -1;
    }

    /* 1. Find existing system message if present */
    size_t  idx;
    json_t* val;
    json_t* sys_msg = NULL;
    json_array_foreach(messages, idx, val)
    {
        if (json_is_object(val)) {
            json_t* r = json_object_get(val, "role");
            if (r != NULL && json_is_string(r) && strcmp(json_string_value(r), "system") == 0) {
                sys_msg = val;
                break;
            }
        }
    }

    if (sys_msg != NULL) {
        if (tmpl->mode == PROMPT_MODE_OVERRIDE) {
            json_object_set_new(sys_msg, "content", json_string(expanded));
        } else {
            json_t*     c = json_object_get(sys_msg, "content");
            const char* orig = (c != NULL && json_is_string(c)) ? json_string_value(c) : "";
            size_t      comb_len = strlen(expanded) + 2 + strlen(orig) + 1;
            char*       combined = malloc(comb_len);
            if (combined == NULL) {
                free(expanded);
                return -1;
            }
            if (tmpl->mode == PROMPT_MODE_APPEND) {
                snprintf(combined, comb_len, "%s\n\n%s", orig, expanded);
            } else {
                snprintf(combined, comb_len, "%s\n\n%s", expanded, orig);
            }
            json_object_set_new(sys_msg, "content", json_string(combined));
            free(combined);
        }
    } else {
        /* No system prompt existed: insert new system message at position 0 */
        json_t* new_sys = json_object();
        json_object_set_new(new_sys, "role", json_string("system"));
        json_object_set_new(new_sys, "content", json_string(expanded));
        json_array_insert_new(messages, 0, new_sys);
    }
    free(expanded);

    /* 2. Optional: prefix / suffix on latest user message */
    if (tmpl->prefix_user_prompt != NULL || tmpl->suffix_user_prompt != NULL) {
        size_t count = json_array_size(messages);
        for (size_t i = count; i > 0; i--) {
            json_t* m = json_array_get(messages, i - 1);
            if (json_is_object(m)) {
                json_t* r = json_object_get(m, "role");
                if (r != NULL && json_is_string(r) && strcmp(json_string_value(r), "user") == 0) {
                    json_t* c = json_object_get(m, "content");
                    if (c != NULL && json_is_string(c)) {
                        const char* orig = json_string_value(c);
                        const char* pre = tmpl->prefix_user_prompt ? tmpl->prefix_user_prompt : "";
                        const char* suf = tmpl->suffix_user_prompt ? tmpl->suffix_user_prompt : "";
                        size_t      ulen = strlen(pre) + strlen(orig) + strlen(suf) + 1;
                        char*       ubuf = malloc(ulen);
                        if (ubuf != NULL) {
                            snprintf(ubuf, ulen, "%s%s%s", pre, orig, suf);
                            json_object_set_new(m, "content", json_string(ubuf));
                            free(ubuf);
                        }
                    }
                    break;
                }
            }
        }
    }

    /* 3. Serialize modified JSON */
    char* serialized = json_dumps(jbody, JSON_COMPACT);
    if (serialized == NULL) {
        return -1;
    }
    *out_modified_json = serialized;
    if (out_len != NULL) {
        *out_len = strlen(serialized);
    }
    return 0;
}
