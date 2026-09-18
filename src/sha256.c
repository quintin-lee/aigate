/** @file sha256.c
 *  @brief SHA-256 helpers (see sha256.h). */
#include "sha256.h"

#include <openssl/evp.h>
#include <string.h>

int
sha256_hex(const void* in, size_t in_len, char out[65])
{
    unsigned char     md[EVP_MAX_MD_SIZE];
    unsigned int      len = 0;
    static const char hexd[] = "0123456789abcdef";

    if (EVP_Digest(in, in_len, md, &len, EVP_sha256(), NULL) != 1) {
        return -1;
    }
    for (unsigned i = 0; i < len; i++) {
        out[i * 2] = hexd[md[i] >> 4];
        out[i * 2 + 1] = hexd[md[i] & 0xF];
    }
    out[len * 2] = '\0';
    return 0;
}

int
sha256_hex_equal(const char a[64], const char b[64])
{
    unsigned long v = 0;
    for (int i = 0; i < 64; i++) {
        v |= (unsigned long)(a[i] - b[i]) & ~0UL;
    }
    return v == 0UL;
}
