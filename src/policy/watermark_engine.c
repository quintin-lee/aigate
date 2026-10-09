#include "watermark_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

char*
watermark_inject(const char* text, size_t len, const watermark_payload_t* payload, size_t* out_len)
{
    if (payload == NULL) {
        return NULL;
    }
    if (text == NULL && len > 0) {
        return NULL;
    }

    /* 1. Assemble 19-byte raw frame */
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

    /* 2. Encode to 228 UTF-8 bytes */
    char wm_utf8[WATERMARK_UTF8_BYTES];
    encode_frame_symbols(frame, wm_utf8);

    /* 3. Allocate new text buffer and append watermark */
    size_t total_len = len + WATERMARK_UTF8_BYTES;
    char*  result = (char*)malloc(total_len + 1);
    if (result == NULL) {
        return NULL;
    }

    if (len > 0) {
        memcpy(result, text, len);
    }
    memcpy(result + len, wm_utf8, WATERMARK_UTF8_BYTES);
    result[total_len] = '\0';

    if (out_len != NULL) {
        *out_len = total_len;
    }
    return result;
}

int
watermark_decode(const char* text, size_t len, watermark_payload_t* out_payload)
{
    if (text == NULL || len < 3 || out_payload == NULL) {
        return -1;
    }
    memset(out_payload, 0, sizeof(*out_payload));

    /* 1. Extract all zero-width symbols from text */
    /* Up to len / 3 symbols possible */
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

    /* 2. Slide window of 76 symbols across the extracted stream */
    for (size_t s = 0; s + WATERMARK_FRAME_SYMBOLS <= n_symbols; s++) {
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
        out_payload->timestamp = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
                                 ((uint32_t)frame[3] << 8) | frame[4];

        out_payload->key_id = ((uint32_t)frame[5] << 24) | ((uint32_t)frame[6] << 16) |
                              ((uint32_t)frame[7] << 8) | frame[8];

        uint64_t st = 0;
        for (int i = 0; i < 8; i++) {
            st = (st << 8) | frame[9 + i];
        }
        out_payload->short_trace = st;
        out_payload->crc_valid = true;

        free(symbols);
        return 0;
    }

    free(symbols);
    return -1;
}
