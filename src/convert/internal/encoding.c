/*
 * qcc — convert internals: execution-character-set codecs (implementation).
 *
 * See encoding.h and ADR-0018. UTF-8 is decoded/encoded per RFC 3629 (the form
 * Unicode and ISO 10646 codify); the surrogate range U+D800..U+DFFF is treated as
 * ill-formed on decode, matching §6.4.3's prohibition on naming a surrogate.
 */
#include "convert/internal/encoding.h"

size_t qcc_encoding_unit_size(qcc_char_encoding enc)
{
    switch (enc) {
    case QCC_ENC_PLAIN:
    case QCC_ENC_UTF8:   return 1;
    case QCC_ENC_CHAR16: return 2;
    case QCC_ENC_WIDE:
    case QCC_ENC_CHAR32: return 4;
    }
    return 1;
}

uint32_t qcc_encoding_unit_max(qcc_char_encoding enc)
{
    switch (enc) {
    case QCC_ENC_PLAIN:
    case QCC_ENC_UTF8:   return 0xFFu;
    case QCC_ENC_CHAR16: return 0xFFFFu;
    case QCC_ENC_WIDE:
    case QCC_ENC_CHAR32: return 0xFFFFFFFFu;
    }
    return 0xFFu;
}

/* Number of bytes a UTF-8 sequence with lead byte `b` claims, or 0 if `b` is not
   a valid lead byte (a continuation byte 10xxxxxx, or an invalid 0xF8..0xFF). */
static int utf8_lead_length(unsigned char b)
{
    if (b < 0x80u)                 return 1; /* 0xxxxxxx */
    if (b >= 0xC2u && b <= 0xDFu)  return 2; /* 110xxxxx (>=C2: no over-long) */
    if (b >= 0xE0u && b <= 0xEFu)  return 3; /* 1110xxxx */
    if (b >= 0xF0u && b <= 0xF4u)  return 4; /* 11110xxx, capped at U+10FFFF */
    return 0;
}

int qcc_utf8_decode(const char *s, size_t len, size_t *pos, uint32_t *cp)
{
    size_t        i = *pos;
    unsigned char b = (unsigned char)s[i];

    int n = utf8_lead_length(b);
    if (n == 1) {
        *cp  = b;
        *pos = i + 1;
        return 1;
    }
    if (n == 0 || i + (size_t)n > len) {
        /* A stray continuation/invalid lead, or a truncated sequence: take one
           byte as Latin-1 so decoding still advances. */
        *cp  = b;
        *pos = i + 1;
        return 0;
    }

    uint32_t acc  = (uint32_t)(b & (0x7Fu >> n)); /* low (7-n) bits of the lead. */
    for (int k = 1; k < n; ++k) {
        unsigned char c = (unsigned char)s[i + (size_t)k];
        if ((c & 0xC0u) != 0x80u) {
            /* Not a 10xxxxxx continuation: reject, consume only the lead byte. */
            *cp  = b;
            *pos = i + 1;
            return 0;
        }
        acc = (acc << 6) | (uint32_t)(c & 0x3Fu);
    }

    /* Reject over-long encodings, surrogates, and out-of-range code points. */
    static const uint32_t min_for_len[5] = { 0, 0, 0x80u, 0x800u, 0x10000u };
    if (acc < min_for_len[n] || (acc >= 0xD800u && acc <= 0xDFFFu) ||
        acc > 0x10FFFFu) {
        *cp  = b;
        *pos = i + 1;
        return 0;
    }

    *cp  = acc;
    *pos = i + (size_t)n;
    return 1;
}

size_t qcc_utf8_encode(uint32_t cp, unsigned char out[4])
{
    if (cp < 0x80u) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (unsigned char)(0xC0u | (cp >> 6));
        out[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (unsigned char)(0xE0u | (cp >> 12));
        out[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (unsigned char)(0xF0u | (cp >> 18));
    out[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 4;
}

size_t qcc_utf16_encode(uint32_t cp, uint16_t out[2])
{
    if (cp < 0x10000u) {
        out[0] = (uint16_t)cp;
        return 1;
    }
    /* Surrogate pair (Unicode 3.8): subtract the plane, split into two 10-bit
       halves biased into the high/low surrogate ranges. */
    uint32_t v = cp - 0x10000u;
    out[0] = (uint16_t)(0xD800u + (v >> 10));
    out[1] = (uint16_t)(0xDC00u + (v & 0x3FFu));
    return 2;
}
