/*
 * qcc — convert internals: execution-character-set codecs (ISO C11 §5.1.1.2,
 * §6.4.4.4, §6.4.5; encoding model in ADR-0018)
 *
 * Responsibility
 * The small Unicode codecs the character-constant and string-literal value paths
 * share: decode one UTF-8 sequence from the (UTF-8) source, encode a code point
 * as UTF-8 or UTF-16, and report the element size of each encoding prefix. These
 * are pure value transforms — no allocation, no diagnostics — so both callers can
 * use them without coupling.
 *
 * Per ADR-0018 the target (x86-64 System V) fixes: narrow / u8 execution set =
 * UTF-8; wchar_t (L) = 32-bit; char16_t (u) = 16-bit UTF-16; char32_t (U) =
 * 32-bit. Storage is little-endian, the target's byte order.
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_ENCODING_H
#define QCC_CONVERT_INTERNAL_ENCODING_H

#include <stddef.h>
#include <stdint.h>

#include "token/token.h"

/*
 * Size in bytes of one code unit of `enc` (ADR-0018): 1 for PLAIN/UTF8, 2 for
 * CHAR16, 4 for WIDE/CHAR32. Total bytes of a decoded string are
 * (str_len + 1) * this, the +1 being the §6.4.5 ¶6 terminator.
 */
size_t qcc_encoding_unit_size(qcc_char_encoding enc);

/*
 * The largest value one code unit of `enc` can hold: 0xFF, 0xFFFF, or
 * 0xFFFFFFFF. Used to range-check a numeric escape (\ooo, \xh…) against the
 * element type (§6.4.4.4 ¶9).
 */
uint32_t qcc_encoding_unit_max(qcc_char_encoding enc);

/*
 * Decode one UTF-8 sequence from s[*pos .. len) into *cp and advance *pos past
 * it. Returns 1 on a well-formed sequence; on a malformed lead/continuation byte
 * (or an over-long/out-of-range encoding) it consumes exactly one byte, sets *cp
 * to that byte (Latin-1 fallback, so decoding always makes progress), and returns
 * 0. The caller decides whether a 0 return warrants a diagnostic.
 */
int qcc_utf8_decode(const char *s, size_t len, size_t *pos, uint32_t *cp);

/*
 * Encode code point `cp` (assumed a valid scalar value, 0..0x10FFFF excluding
 * surrogates — UCNs are validated by the escape decoder) as UTF-8 into out[0..4)
 * and return the number of bytes written (1..4).
 */
size_t qcc_utf8_encode(uint32_t cp, unsigned char out[4]);

/*
 * Encode code point `cp` as UTF-16 into out[0..2) and return the number of 16-bit
 * units written: 1 for the BMP, 2 (a surrogate pair) for U+10000..U+10FFFF.
 */
size_t qcc_utf16_encode(uint32_t cp, uint16_t out[2]);

#endif /* QCC_CONVERT_INTERNAL_ENCODING_H */
