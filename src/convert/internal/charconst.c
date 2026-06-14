/*
 * qcc — convert internals: character-constant evaluation (implementation).
 *
 * See charconst.h and ADR-0018. The lexeme is a complete character constant
 * (§6.4.4.4): an optional prefix (L/u/U), a `'`, one or more c-chars or escape
 * sequences, and a closing `'`. We read the prefix, decode the body, and fold the
 * decoded units into the constant's integer value, following the encoding model
 * of ADR-0018 (narrow execution set = UTF-8; wchar_t/char16_t/char32_t are the
 * x86-64 System V widths).
 */
#include "convert/internal/charconst.h"

#include "convert/internal/encoding.h"
#include "convert/internal/escape.h"

/*
 * Read the optional encoding prefix of a character constant (§6.4.4.4 ¶2): L,
 * u, or U immediately before the opening quote. (u8 is a string-only prefix,
 * §6.4.5 ¶3, so it does not appear here.) Returns the encoding and sets
 * *body_start to the index just past the opening `'`.
 */
static qcc_char_encoding char_prefix(const char *s, size_t n, size_t *body_start)
{
    size_t            i   = 0;
    qcc_char_encoding enc = QCC_ENC_PLAIN;
    if (n >= 2 && s[1] == '\'') {
        if (s[0] == 'L') {
            enc = QCC_ENC_WIDE;
            i   = 1;
        } else if (s[0] == 'u') {
            enc = QCC_ENC_CHAR16;
            i   = 1;
        } else if (s[0] == 'U') {
            enc = QCC_ENC_CHAR32;
            i   = 1;
        }
    }
    *body_start = i + 1; /* Step past the opening quote at index i. */
    return enc;
}

/*
 * Value of a plain (type int) character constant. The execution set is UTF-8
 * (ADR-0018), so the body is a byte sequence: a c-char is its own bytes, a UCN is
 * UTF-8-encoded, and a numeric escape is a raw byte. §6.4.4.4 ¶10: a single byte
 * is the (sign-extended, char being signed) value; more than one byte is an
 * implementation-defined multi-character constant, which we pack big-endian into
 * an int as the common toolchains do.
 */
static qcc_status eval_plain(const char *s, size_t body, size_t end,
                             const qcc_source *src, size_t offset,
                             qcc_diag_sink *diags, uint64_t *out_value)
{
    uint64_t acc   = 0;
    size_t   count = 0;
    size_t   pos   = body;

    while (pos < end) {
        if (s[pos] == '\\') {
            uint32_t        v;
            qcc_escape_kind kind;
            qcc_status st = qcc_decode_escape(s, end, &pos, src, offset, diags,
                                              &v, &kind);
            if (st != QCC_OK) {
                return st;
            }
            if (kind == QCC_ESCAPE_UCN) {
                unsigned char buf[4];
                size_t        k = qcc_utf8_encode(v, buf);
                for (size_t b = 0; b < k; ++b) {
                    acc = (acc << 8) | buf[b];
                    ++count;
                }
            } else {
                if (v > 0xFFu) {
                    st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                       "escape sequence out of range for "
                                       "character constant");
                    if (st != QCC_OK) {
                        return st;
                    }
                }
                acc = (acc << 8) | (v & 0xFFu);
                ++count;
            }
        } else {
            acc = (acc << 8) | (unsigned char)s[pos];
            ++count;
            ++pos;
        }
    }

    if (count == 0) {
        *out_value = 0;
        return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                             "empty character constant");
    }

    uint32_t w = (uint32_t)acc; /* Low 4 bytes: the last 4 chars if more. */
    if (count == 1) {
        /* char is signed on the target: a high-bit byte sign-extends to int. */
        *out_value = (uint64_t)(int64_t)(int8_t)(uint8_t)w;
        return QCC_OK;
    }

    /* Multi-character constant: implementation-defined (§6.4.4.4 ¶10). */
    qcc_status st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, 1,
                                  "multi-character character constant");
    if (st != QCC_OK) {
        return st;
    }
    if (count > 4) {
        st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, 1,
                           "character constant too long for its type");
        if (st != QCC_OK) {
            return st;
        }
    }
    *out_value = (uint64_t)(int64_t)(int32_t)w; /* int reinterpretation. */
    return QCC_OK;
}

/*
 * Value of a wide (L/u/U) character constant. Each c-char or UCN is one code
 * point (UTF-8-decoded from the source); a numeric escape is a raw code unit. The
 * value is that single unit, range-checked against the element type (§6.4.4.4
 * ¶11). More than one is an implementation-defined multi-character wide constant;
 * we keep the last unit with a warning.
 */
static qcc_status eval_wide(qcc_char_encoding enc, const char *s, size_t body,
                            size_t end, const qcc_source *src, size_t offset,
                            qcc_diag_sink *diags, uint64_t *out_value)
{
    uint32_t last  = 0;
    size_t   count = 0;
    size_t   pos   = body;
    uint32_t umax  = qcc_encoding_unit_max(enc);

    while (pos < end) {
        uint32_t value;
        if (s[pos] == '\\') {
            qcc_escape_kind kind;
            qcc_status st = qcc_decode_escape(s, end, &pos, src, offset, diags,
                                              &value, &kind);
            if (st != QCC_OK) {
                return st;
            }
            /* Both a UCN and a numeric escape contribute exactly one wide unit. */
            if (value > umax) {
                st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                   "escape sequence out of range for "
                                   "character constant");
                if (st != QCC_OK) {
                    return st;
                }
                value &= umax;
            }
        } else if ((unsigned char)s[pos] < 0x80u) {
            value = (unsigned char)s[pos];
            ++pos;
        } else {
            uint32_t cp;
            (void)qcc_utf8_decode(s, end, &pos, &cp);
            value = cp;
            if (value > umax) {
                qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset,
                                              1, "character constant does not fit "
                                              "in its type");
                if (st != QCC_OK) {
                    return st;
                }
                value &= umax;
            }
        }
        last = value;
        ++count;
    }

    if (count == 0) {
        *out_value = 0;
        return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                             "empty character constant");
    }
    if (count > 1) {
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, 1,
                                      "multi-character character constant");
        if (st != QCC_OK) {
            return st;
        }
    }

    /* wchar_t is signed on the target; char16_t/char32_t are unsigned. */
    if (enc == QCC_ENC_WIDE) {
        *out_value = (uint64_t)(int64_t)(int32_t)last;
    } else {
        *out_value = (uint64_t)last;
    }
    return QCC_OK;
}

qcc_status qcc_eval_char(const char *s, size_t n, const qcc_source *src,
                         size_t offset, qcc_diag_sink *diags,
                         uint64_t *out_value, qcc_char_encoding *out_encoding)
{
    size_t            body;
    qcc_char_encoding enc = char_prefix(s, n, &body);
    *out_encoding         = enc;
    *out_value            = 0;

    /* The body runs up to the closing quote; if the lexeme is unterminated (the
       lexer has already diagnosed that) decode to the end. */
    size_t end = (n > body && s[n - 1] == '\'') ? n - 1 : n;
    if (end < body) {
        end = body;
    }

    if (enc == QCC_ENC_PLAIN) {
        return eval_plain(s, body, end, src, offset, diags, out_value);
    }
    return eval_wide(enc, s, body, end, src, offset, diags, out_value);
}
