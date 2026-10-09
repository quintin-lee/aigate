/**
 * @file watermark_engine.h
 * @brief Zero-width Unicode steganographic watermark injection and extraction engine.
 */
#ifndef AIGATE_WATERMARK_ENGINE_H
#define AIGATE_WATERMARK_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATERMARK_MAGIC_BYTE 0x57
#define WATERMARK_FRAME_BYTES 19
#define WATERMARK_FRAME_SYMBOLS 76
#define WATERMARK_UTF8_BYTES (WATERMARK_FRAME_SYMBOLS * 3)

typedef struct {
    uint32_t timestamp;   /**< Generation timestamp (epoch seconds) */
    uint32_t key_id;      /**< Client API Key identifier */
    uint64_t short_trace; /**< 64-bit fingerprint of distributed Trace ID */
    bool     crc_valid;   /**< True if CRC-16 checksum passed */
} watermark_payload_t;

/**
 * @brief Inject zero-width steganographic watermark frame into text.
 * @param text Original source text.
 * @param len Byte length of source text.
 * @param payload Metadata payload to encode.
 * @param[out] out_len Written with new byte length (len + 228).
 * @return Newly allocated watermarked string (caller frees), or NULL on OOM.
 */
char*
watermark_inject(const char* text, size_t len, const watermark_payload_t* payload, size_t* out_len);

/**
 * @brief Scan and decode steganographic zero-width watermark from arbitrary text.
 * @param text Analyzed text containing potential zero-width characters.
 * @param len Byte length of text.
 * @param[out] out_payload Written with extracted metadata if found and CRC valid.
 * @return 0 on successful decode with valid CRC; -1 if no valid frame found.
 */
int watermark_decode(const char* text, size_t len, watermark_payload_t* out_payload);

#ifdef __cplusplus
}
#endif

#endif /* AIGATE_WATERMARK_ENGINE_H */
