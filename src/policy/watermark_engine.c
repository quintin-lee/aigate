#include "watermark_engine.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 4 zero-width Unicode characters (each 3 bytes in UTF-8) */
static const char ZW_SYMBOLS[4][3] = {
    {'\xE2', '\x80', '\x8B'}, /* 00: U+200B Zero-Width Space */
    {'\xE2', '\x80', '\x8C'}, /* 01: U+200C Zero-Width Non-Joiner */
    {'\xE2', '\x80', '\x8D'}, /* 10: U+200D Zero-Width Joiner */
    {'\xEF', '\xBB', '\xBF'}  /* 11: U+FEFF Zero-Width No-Break Space / BOM */
};

/**
 * @brief Standard CRC-16-CCITT computation (poly 0x1021, init 0xFFFF).
 */
static uint16_t
crc16_ccitt(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/**
 * @brief Encode 19-byte raw frame buffer into 76 zero-width symbols (228 UTF-8 bytes).
 */
static void
encode_frame_symbols(const uint8_t* frame, char* out_utf8)
{
    size_t out_idx = 0;
    for (size_t i = 0; i < WATERMARK_FRAME_BYTES; i++) {
        uint8_t byte = frame[i];
        for (int shift = 6; shift >= 0; shift -= 2) {
            uint8_t sym = (byte >> shift) & 0x03;
            memcpy(&out_utf8[out_idx], ZW_SYMBOLS[sym], 3);
            out_idx += 3;
        }
    }
}

/**
 * @brief Encode payload metadata into 228 UTF-8 bytes.
 */
static void
encode_payload_to_utf8(const watermark_payload_t* payload, char* out_wm_utf8)
{
    uint8_t frame[WATERMARK_FRAME_BYTES];
    frame[0] = WATERMARK_MAGIC_BYTE;

    /* Timestamp (32-bit big endian) */
    frame[1] = (uint8_t)((payload->timestamp >> 24) & 0xFF);
    frame[2] = (uint8_t)((payload->timestamp >> 16) & 0xFF);
    frame[3] = (uint8_t)((payload->timestamp >> 8) & 0xFF);
    frame[4] = (uint8_t)(payload->timestamp & 0xFF);

    /* Key ID (32-bit big endian) */
    frame[5] = (uint8_t)((payload->key_id >> 24) & 0xFF);
    frame[6] = (uint8_t)((payload->key_id >> 16) & 0xFF);
    frame[7] = (uint8_t)((payload->key_id >> 8) & 0xFF);
    frame[8] = (uint8_t)(payload->key_id & 0xFF);

    /* Short Trace (64-bit big endian) */
    for (int i = 0; i < 8; i++) {
        frame[9 + i] = (uint8_t)((payload->short_trace >> (56 - i * 8)) & 0xFF);
    }

    /* CRC-16 over bytes 0..16 */
    uint16_t crc = crc16_ccitt(frame, 17);
    frame[17] = (uint8_t)((crc >> 8) & 0xFF);
    frame[18] = (uint8_t)(crc & 0xFF);

    encode_frame_symbols(frame, out_wm_utf8);
}

/**
 * @brief Check whether character at index i is a sentence punctuation anchor.
 */
static bool
is_punctuation_anchor(const char* text, size_t i, size_t len, size_t* advance)
{
    if (i >= len) {
        return false;
    }
    unsigned char c = (unsigned char)text[i];

    /* ASCII anchors: '.', '?', '!', ';', '\n' */
    if (c == '?' || c == '!' || c == ';' || c == '\n') {
        *advance = 1;
        return true;
    }
    if (c == '.') {
        /* Avoid decimal numbers like 3.14 */
        if (i + 1 < len) {
            unsigned char next = (unsigned char)text[i + 1];
            if (next >= '0' && next <= '9') {
                return false;
            }
        }
        *advance = 1;
        return true;
    }

    /* UTF-8 Chinese/full-width anchors */
    if (i + 3 <= len) {
        unsigned char b0 = (unsigned char)text[i];
        unsigned char b1 = (unsigned char)text[i + 1];
        unsigned char b2 = (unsigned char)text[i + 2];
        /* 。: E3 80 82 */
        if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x82) {
            *advance = 3;
            return true;
        }
        /* ！: EF BC 81 */
        if (b0 == 0xEF && b1 == 0xBC && b2 == 0x81) {
            *advance = 3;
            return true;
        }
        /* ？: EF BC 9F */
        if (b0 == 0xEF && b1 == 0xBC && b2 == 0x9F) {
            *advance = 3;
            return true;
        }
        /* ；: EF BC 9B */
        if (b0 == 0xEF && b1 == 0xBC && b2 == 0x9B) {
            *advance = 3;
            return true;
        }
    }
    return false;
}

char*
watermark_inject_multi_tile(const char*                text,
                            size_t                     len,
                            const watermark_payload_t* payload,
                            size_t                     min_interval,
                            size_t*                    out_len)
{
    if (payload == NULL) {
        return NULL;
    }
    if (text == NULL && len > 0) {
        return NULL;
    }
    if (min_interval < 20) {
        min_interval = 20;
    }

    char wm_utf8[WATERMARK_UTF8_BYTES];
    encode_payload_to_utf8(payload, wm_utf8);

    /* Collect anchor insertion positions */
    size_t anchors[256];
    size_t n_anchors = 0;
    size_t last_pos = 0;

    for (size_t i = 0; i < len;) {
        size_t adv = 0;
        if (is_punctuation_anchor(text, i, len, &adv)) {
            size_t insert_pos = i + adv;
            if (insert_pos - last_pos >= min_interval && n_anchors < 256) {
                anchors[n_anchors++] = insert_pos;
                last_pos = insert_pos;
            }
            i += adv;
        } else {
            i++;
        }
    }

    /* Fallback: if no anchor point met interval criteria, inject 1 tile at the end */
    if (n_anchors == 0) {
        anchors[0] = len;
        n_anchors = 1;
    }

    /* Allocate buffer */
    size_t total_len = len + n_anchors * WATERMARK_UTF8_BYTES;
    char*  result = (char*)malloc(total_len + 1);
    if (result == NULL) {
        return NULL;
    }

    size_t src_pos = 0;
    size_t dst_pos = 0;

    for (size_t a = 0; a < n_anchors; a++) {
        size_t chunk_len = anchors[a] - src_pos;
        if (chunk_len > 0) {
            memcpy(result + dst_pos, text + src_pos, chunk_len);
            dst_pos += chunk_len;
            src_pos = anchors[a];
        }
        memcpy(result + dst_pos, wm_utf8, WATERMARK_UTF8_BYTES);
        dst_pos += WATERMARK_UTF8_BYTES;
    }

    /* Remaining trailing text if any */
    if (src_pos < len) {
        size_t rem = len - src_pos;
        memcpy(result + dst_pos, text + src_pos, rem);
        dst_pos += rem;
    }

    result[total_len] = '\0';
    if (out_len != NULL) {
        *out_len = total_len;
    }
    return result;
}

char*
watermark_inject(const char* text, size_t len, const watermark_payload_t* payload, size_t* out_len)
{
    return watermark_inject_multi_tile(text, len, payload, 100, out_len);
}

int
watermark_decode_all(const char*          text,
                     size_t               len,
                     watermark_payload_t* out_payloads,
                     int                  cap,
                     int*                 out_count)
{
    if (text == NULL || len < 3 || out_payloads == NULL || cap <= 0 || out_count == NULL) {
        return -1;
    }
    *out_count = 0;

    /* 1. Extract all zero-width symbols from text */
    size_t   max_symbols = len / 3;
    uint8_t* symbols = (uint8_t*)malloc(max_symbols);
    if (symbols == NULL) {
        return -1;
    }

    size_t n_symbols = 0;
    for (size_t i = 0; i + 3 <= len;) {
        unsigned char b0 = (unsigned char)text[i];
        unsigned char b1 = (unsigned char)text[i + 1];
        unsigned char b2 = (unsigned char)text[i + 2];

        if (b0 == 0xE2 && b1 == 0x80) {
            if (b2 == 0x8B) {
                symbols[n_symbols++] = 0;
                i += 3;
                continue;
            } else if (b2 == 0x8C) {
                symbols[n_symbols++] = 1;
                i += 3;
                continue;
            } else if (b2 == 0x8D) {
                symbols[n_symbols++] = 2;
                i += 3;
                continue;
            }
        } else if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            symbols[n_symbols++] = 3;
            i += 3;
            continue;
        }
        i++;
    }

    if (n_symbols < WATERMARK_FRAME_SYMBOLS) {
        free(symbols);
        return -1;
    }

    int found = 0;
    /* 2. Slide window of 76 symbols across the extracted stream */
    for (size_t s = 0; s + WATERMARK_FRAME_SYMBOLS <= n_symbols && found < cap; s++) {
        uint8_t frame[WATERMARK_FRAME_BYTES];
        for (size_t b = 0; b < WATERMARK_FRAME_BYTES; b++) {
            size_t  sym_base = s + b * 4;
            uint8_t byte = (uint8_t)((symbols[sym_base] << 6) | (symbols[sym_base + 1] << 4) |
                                     (symbols[sym_base + 2] << 2) | symbols[sym_base + 3]);
            frame[b] = byte;
        }

        /* Check Magic */
        if (frame[0] != WATERMARK_MAGIC_BYTE) {
            continue;
        }

        /* Check CRC-16 */
        uint16_t expected_crc = ((uint16_t)frame[17] << 8) | frame[18];
        uint16_t actual_crc = crc16_ccitt(frame, 17);
        if (expected_crc != actual_crc) {
            continue;
        }

        /* Valid frame found! Unpack fields */
        watermark_payload_t* p = &out_payloads[found];
        p->timestamp = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
                       ((uint32_t)frame[3] << 8) | frame[4];

        p->key_id = ((uint32_t)frame[5] << 24) | ((uint32_t)frame[6] << 16) |
                    ((uint32_t)frame[7] << 8) | frame[8];

        uint64_t st = 0;
        for (int i = 0; i < 8; i++) {
            st = (st << 8) | frame[9 + i];
        }
        p->short_trace = st;
        p->crc_valid = true;

        found++;
        /* Advance window to skip past the rest of this frame */
        s += WATERMARK_FRAME_SYMBOLS - 1;
    }

    free(symbols);
    if (found > 0) {
        *out_count = found;
        return 0;
    }
    return -1;
}

int
watermark_decode(const char* text, size_t len, watermark_payload_t* out_payload)
{
    if (out_payload == NULL) {
        return -1;
    }
    int cnt = 0;
    return watermark_decode_all(text, len, out_payload, 1, &cnt);
}
