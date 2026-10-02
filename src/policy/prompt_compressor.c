/**
 * @file prompt_compressor.c
 * @brief Implementation of prompt compression, token estimation, and whitespace sanitization.
 */

#include "policy/prompt_compressor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t
compressor_estimate_tokens(const char* text, size_t len)
{
    if (!text || len == 0) {
        return 0;
    }

    uint32_t words = 0;
    uint32_t multibyte_chars = 0;
    bool     in_word = false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) {
            /* UTF-8 multi-byte sequence leading byte check */
            if ((c & 0xC0) != 0x80) {
                multibyte_chars++;
            }
            if (in_word) {
                in_word = false;
            }
        } else if (isspace(c) || ispunct(c)) {
            if (in_word) {
                words++;
                in_word = false;
            }
            if (ispunct(c)) {
                /* Common punctuation counts as separate token in BPE */
                words++;
            }
        } else {
            in_word = true;
        }
    }
    if (in_word) {
        words++;
    }

    /* English tokens: ~1.25 to 1.3 per word; CJK tokens: ~1.0 to 1.5 per char */
    uint32_t est = (uint32_t)((double)words * 1.3) + multibyte_chars;
    return est > 0 ? est : 1;
}

size_t
compressor_sanitize_whitespace(
    const char* src, size_t src_len, char* dst, size_t dst_cap, bool preserve_code)
{
    if (!src || src_len == 0 || !dst || dst_cap == 0) {
        return 0;
    }

    bool   in_code_block = false;
    size_t r = 0;
    size_t w = 0;
    int    consecutive_newlines = 0;
    bool   in_whitespace_run = false;

    while (r < src_len && w + 1 < dst_cap) {
        /* Check for code block delimiter ``` at start of line or position */
        if (preserve_code && (r == 0 || src[r - 1] == '\n') && (r + 2 < src_len) && src[r] == '`' &&
            src[r + 1] == '`' && src[r + 2] == '`') {
            in_code_block = !in_code_block;
            consecutive_newlines = 0;
            in_whitespace_run = false;
            /* Copy the ``` */
            for (int k = 0; k < 3 && r < src_len && w + 1 < dst_cap; k++) {
                dst[w++] = src[r++];
            }
            continue;
        }

        char c = src[r];

        if (in_code_block) {
            /* Inside code block: preserve every character verbatim */
            dst[w++] = c;
            r++;
            continue;
        }

        /* Outside code block: collapse excess whitespace and newlines */
        if (c == '\r') {
            /* Ignore CR for canonical LF */
            r++;
            continue;
        }

        if (c == '\n') {
            consecutive_newlines++;
            in_whitespace_run = false;
            if (consecutive_newlines <= 2) {
                dst[w++] = '\n';
            }
            r++;
            continue;
        }

        consecutive_newlines = 0;

        if (c == ' ' || c == '\t') {
            if (!in_whitespace_run) {
                dst[w++] = ' ';
                in_whitespace_run = true;
            }
            r++;
            continue;
        }

        in_whitespace_run = false;
        dst[w++] = c;
        r++;
    }

    dst[w] = '\0';
    return w;
}
