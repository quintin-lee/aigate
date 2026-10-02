/**
 * @file cache_optimizer.c
 * @brief Implementation of prompt cache prefix alignment and optimization engine.
 */

#include "policy/cache_optimizer.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Helper structure used for sorting tool and function definitions.
 */
typedef struct {
    json_t*     elem; /**< Underlying JSON object pointer */
    const char* name; /**< Extracted tool or function name */
} tool_entry_t;

/**
 * @brief Comparator for sorting tool entries alphabetically by name.
 */
static int
tool_entry_cmp(const void* a, const void* b)
{
    const tool_entry_t* ea = (const tool_entry_t*)a;
    const tool_entry_t* eb = (const tool_entry_t*)b;
    return strcmp(ea->name, eb->name);
}

bool
cache_optimizer_sort_tools(json_t* root)
{
    if (!root || !json_is_object(root)) {
        return false;
    }

    json_t* tools = json_object_get(root, "tools");
    if (!tools || !json_is_array(tools)) {
        tools = json_object_get(root, "functions");
        if (!tools || !json_is_array(tools)) {
            return false;
        }
    }

    size_t count = json_array_size(tools);
    if (count <= 1) {
        return false;
    }

    tool_entry_t* entries = (tool_entry_t*)malloc(sizeof(tool_entry_t) * count);
    if (!entries) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        json_t*     item = json_array_get(tools, i);
        const char* name = NULL;

        if (json_is_object(item)) {
            json_t* fn = json_object_get(item, "function");
            if (json_is_object(fn)) {
                json_t* fn_name = json_object_get(fn, "name");
                if (json_is_string(fn_name)) {
                    name = json_string_value(fn_name);
                }
            }
            if (!name) {
                json_t* item_name = json_object_get(item, "name");
                if (json_is_string(item_name)) {
                    name = json_string_value(item_name);
                }
            }
        }
        entries[i].elem = item;
        entries[i].name = name ? name : "";
    }

    /* Check if already sorted */
    bool already_sorted = true;
    for (size_t i = 1; i < count; i++) {
        if (strcmp(entries[i - 1].name, entries[i].name) > 0) {
            already_sorted = false;
            break;
        }
    }

    if (already_sorted) {
        free(entries);
        return false;
    }

    /* Sort entries stably */
    qsort(entries, count, sizeof(tool_entry_t), tool_entry_cmp);

    /* Rebuild array in sorted order */
    for (size_t i = 0; i < count; i++) {
        json_incref(entries[i].elem);
    }
    json_array_clear(tools);
    for (size_t i = 0; i < count; i++) {
        json_array_append_new(tools, entries[i].elem);
    }

    free(entries);
    return true;
}

size_t
cache_optimizer_normalize_whitespace(const char* in, size_t in_len, char* out, size_t out_sz)
{
    if (!in || in_len == 0 || !out || out_sz == 0) {
        if (out && out_sz > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    size_t r = 0;
    size_t w = 0;

    /* Skip leading whitespace */
    while (r < in_len && (in[r] == ' ' || in[r] == '\t' || in[r] == '\r')) {
        r++;
    }

    int consecutive_newlines = 0;
    int consecutive_spaces = 0;

    while (r < in_len && w + 2 < out_sz) {
        char c = in[r++];

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            /* Trim trailing whitespace on current line before newline */
            while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
                w--;
            }
            consecutive_spaces = 0;
            if (consecutive_newlines < 2) {
                out[w++] = '\n';
                consecutive_newlines++;
            }
            continue;
        }

        if (c == ' ' || c == '\t') {
            if (consecutive_newlines > 0) {
                /* Skip spaces right after newlines */
                continue;
            }
            if (consecutive_spaces == 0) {
                out[w++] = ' ';
                consecutive_spaces++;
            }
            continue;
        }

        /* Regular non-whitespace character */
        consecutive_newlines = 0;
        consecutive_spaces = 0;
        out[w++] = c;
    }

    /* Trim trailing spaces before newlines or end */
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) {
        w--;
    }

    out[w] = '\0';
    return w;
}

/**
 * @brief Checks if a string prefix matches common volatile date/time or session patterns.
 */
static bool
is_dynamic_header_prefix(const char* p, size_t len)
{
    if (len < 8) {
        return false;
    }

    if (strncasecmp(p, "today is", 8) == 0) {
        return true;
    }
    if (strncasecmp(p, "current date", 12) == 0 || strncasecmp(p, "current time", 12) == 0) {
        return true;
    }
    if (strncasecmp(p, "session id:", 11) == 0 || strncasecmp(p, "session_id:", 11) == 0 ||
        strncasecmp(p, "session:", 8) == 0) {
        return true;
    }
    if (strncasecmp(p, "request id:", 11) == 0 || strncasecmp(p, "request_id:", 11) == 0) {
        return true;
    }

    /* Check for leading ISO date: YYYY-MM-DD or YYYY/MM/DD */
    if (len >= 10 && isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
        isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
        (p[4] == '-' || p[4] == '/') && isdigit((unsigned char)p[5]) &&
        isdigit((unsigned char)p[6]) && (p[7] == '-' || p[7] == '/') &&
        isdigit((unsigned char)p[8]) && isdigit((unsigned char)p[9])) {
        return true;
    }

    return false;
}

bool
cache_optimizer_sink_dynamic_system(const char* in, size_t in_len, char* out, size_t out_sz)
{
    if (!in || in_len == 0 || !out || out_sz == 0) {
        return false;
    }

    /* Skip leading whitespace */
    size_t start = 0;
    while (start < in_len && isspace((unsigned char)in[start])) {
        start++;
    }

    if (!is_dynamic_header_prefix(in + start, in_len - start)) {
        return false;
    }

    /* Find end of the first dynamic sentence or line */
    size_t end = start;
    while (end < in_len) {
        if (in[end] == '\n') {
            break;
        }
        if (in[end] == '.' && (end + 1 >= in_len || isspace((unsigned char)in[end + 1]))) {
            end++; /* include the period */
            break;
        }
        end++;
    }

    /* Extract dynamic slice, trimming trailing period and whitespace */
    size_t dyn_start = start;
    size_t dyn_end = end;
    while (dyn_end > dyn_start &&
           (in[dyn_end - 1] == '.' || isspace((unsigned char)in[dyn_end - 1]))) {
        dyn_end--;
    }
    size_t dyn_len = dyn_end - dyn_start;
    if (dyn_len == 0) {
        return false;
    }

    /* Find start of remaining static content */
    size_t static_start = end;
    while (static_start < in_len && isspace((unsigned char)in[static_start])) {
        static_start++;
    }

    if (static_start >= in_len) {
        /* No static content following the dynamic header */
        return false;
    }

    size_t static_len = in_len - static_start;

    /* Format into out: static content first, then [Runtime Context: <dyn>]\n */
    int written = snprintf(out,
                           out_sz,
                           "%.*s\n\n[Runtime Context: %.*s]\n",
                           (int)static_len,
                           in + static_start,
                           (int)dyn_len,
                           in + dyn_start);

    return (written > 0 && (size_t)written < out_sz);
}
