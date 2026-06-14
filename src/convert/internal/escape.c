/*
 * qcc — convert internals: escape-sequence decoding (implementation).
 *
 * See escape.h. The caller positions *pos on the backslash; we classify the
 * character after it (§6.4.4.4 ¶4, §6.4.3) and consume the whole sequence.
 */
#include "convert/internal/escape.h"

/* The value of a simple escape letter (§6.4.4.4 ¶4), or -1 if `c` is not one. */
static int simple_escape(char c)
{
    switch (c) {
    case '\'': return '\'';
    case '"':  return '"';
    case '?':  return '?';
    case '\\': return '\\';
    case 'a':  return 7;   /* alert      */
    case 'b':  return 8;   /* backspace  */
    case 'f':  return 12;  /* form feed  */
    case 'n':  return 10;  /* newline    */
    case 'r':  return 13;  /* carriage return */
    case 't':  return 9;   /* tab        */
    case 'v':  return 11;  /* vertical tab */
    default:   return -1;
    }
}

/* Hex digit value of `c`, or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

qcc_status qcc_decode_escape(const char *body, size_t len, size_t *pos,
                             const qcc_source *src, size_t offset,
                             qcc_diag_sink *diags, uint32_t *out_value,
                             qcc_escape_kind *out_kind)
{
    *out_kind = QCC_ESCAPE_VALUE; /* All but a UCN are raw values; reset below. */

    size_t i = *pos + 1; /* Step over the backslash. */
    if (i >= len) {
        *out_value = (uint32_t)'\\';
        *pos       = i;
        return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                             "incomplete escape sequence");
    }

    char c  = body[i];
    int  se = simple_escape(c);
    if (se >= 0) {
        *out_value = (uint32_t)se;
        *pos       = i + 1;
        return QCC_OK;
    }

    if (c >= '0' && c <= '7') {
        /* Octal escape: one to three octal digits (§6.4.4.4 ¶4). */
        uint32_t v = 0;
        int      k = 0;
        while (i < len && k < 3 && body[i] >= '0' && body[i] <= '7') {
            v = v * 8u + (uint32_t)(body[i] - '0');
            ++i;
            ++k;
        }
        *out_value = v;
        *pos       = i;
        return QCC_OK;
    }

    if (c == 'x') {
        /* Hexadecimal escape: one or more hex digits, greedy (§6.4.4.4 ¶4). */
        ++i;
        if (i >= len || hex_value(body[i]) < 0) {
            *out_value = (uint32_t)'x';
            *pos       = i;
            return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                 "\\x used with no following hex digits");
        }
        uint32_t v = 0;
        while (i < len) {
            int h = hex_value(body[i]);
            if (h < 0) {
                break;
            }
            v = v * 16u + (uint32_t)h;
            ++i;
        }
        *out_value = v;
        *pos       = i;
        return QCC_OK;
    }

    if (c == 'u' || c == 'U') {
        /* Universal character name: \u + 4 hex, \U + 8 hex (§6.4.3). */
        *out_kind   = QCC_ESCAPE_UCN;
        int ndigits = (c == 'u') ? 4 : 8;
        ++i;
        uint32_t v   = 0;
        int      got = 0;
        while (i < len && got < ndigits) {
            int h = hex_value(body[i]);
            if (h < 0) {
                break;
            }
            v = v * 16u + (uint32_t)h;
            ++i;
            ++got;
        }
        *pos       = i;
        *out_value = v;
        if (got != ndigits) {
            return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                 "incomplete universal character name");
        }
        /* §6.4.3 ¶2: a UCN shall not name a surrogate or a value above 0x10FFFF. */
        if ((v >= 0xD800u && v <= 0xDFFFu) || v > 0x10FFFFu) {
            return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                 "universal character name is not a valid "
                                 "code point");
        }
        return QCC_OK;
    }

    /* Any other letter is not a defined escape (§6.4.4.4): undefined behavior;
       diagnose and take the character literally. */
    *out_value = (uint32_t)(unsigned char)c;
    *pos       = i + 1;
    return qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, 1,
                         "unknown escape sequence '\\%c'", c);
}
