/*
 * qcc — convert internals: string-literal evaluation (implementation).
 *
 * See strlit.h and ADR-0018. We accumulate the decoded code units of every piece
 * into one growable byte buffer (seed allocator, transient), append the §6.4.5 ¶6
 * terminator, then copy the whole thing into the caller's arena. Narrow (PLAIN /
 * u8) pieces decode to UTF-8 bytes; wide (L / u / U) pieces decode to 16- or
 * 32-bit units. A numeric escape is a raw code unit; a UCN and a plain character
 * are code points encoded into the target encoding.
 */
#include "convert/internal/strlit.h"

#include <stdlib.h>
#include <string.h>

#include "convert/internal/encoding.h"
#include "convert/internal/escape.h"

/* A transient growable byte buffer; bytes are the units in target byte order. */
typedef struct sbuf {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} sbuf;

static int sbuf_reserve(sbuf *b, size_t extra)
{
    if (b->len + extra <= b->cap) {
        return 1;
    }
    size_t ncap = (b->cap == 0) ? 64u : b->cap;
    while (ncap < b->len + extra) {
        ncap *= 2u;
    }
    unsigned char *grown = (unsigned char *)realloc(b->data, ncap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap  = ncap;
    return 1;
}

static int sbuf_push_bytes(sbuf *b, const unsigned char *p, size_t n)
{
    if (!sbuf_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

/* Append `value` as one little-endian code unit of `unit_size` bytes (ADR-0018:
   the x86-64 target is little-endian, so this is target memory order). */
static int sbuf_push_unit(sbuf *b, uint32_t value, size_t unit_size)
{
    unsigned char tmp[4];
    for (size_t i = 0; i < unit_size; ++i) {
        tmp[i] = (unsigned char)((value >> (8u * i)) & 0xFFu);
    }
    return sbuf_push_bytes(b, tmp, unit_size);
}

/* Append code point `cp` to a wide buffer: UTF-16 for char16_t, a single 32-bit
   unit for wchar_t/char32_t. */
static int push_codepoint_wide(sbuf *b, qcc_char_encoding enc, uint32_t cp)
{
    if (enc == QCC_ENC_CHAR16) {
        uint16_t units[2];
        size_t   k = qcc_utf16_encode(cp, units);
        for (size_t i = 0; i < k; ++i) {
            if (!sbuf_push_unit(b, units[i], 2)) {
                return 0;
            }
        }
        return 1;
    }
    return sbuf_push_unit(b, cp, 4); /* WIDE / CHAR32 */
}

/*
 * Read the optional encoding prefix of a string literal (§6.4.5 ¶3): u8, L, u,
 * or U before the opening quote. Returns the encoding and sets *body_start past
 * the opening `"`.
 */
static qcc_char_encoding string_prefix(const char *s, size_t n, size_t *body_start)
{
    size_t            i   = 0;
    qcc_char_encoding enc = QCC_ENC_PLAIN;
    if (n >= 3 && s[0] == 'u' && s[1] == '8' && s[2] == '"') {
        enc = QCC_ENC_UTF8;
        i   = 2;
    } else if (n >= 2 && s[1] == '"') {
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

/* Decode one piece's body into `b`, encoding into the resolved `enc`. */
static qcc_status decode_piece(sbuf *b, qcc_char_encoding enc, const char *s,
                               size_t n, const qcc_source *src, size_t offset,
                               qcc_diag_sink *diags)
{
    size_t body;
    (void)string_prefix(s, n, &body); /* The piece's own prefix only locates the
                                         body; output uses the resolved enc. */
    size_t end = (n > body && s[n - 1] == '"') ? n - 1 : n;
    if (end < body) {
        end = body;
    }

    int narrow = (enc == QCC_ENC_PLAIN || enc == QCC_ENC_UTF8);

    size_t pos = body;
    while (pos < end) {
        if (s[pos] == '\\') {
            uint32_t        v;
            qcc_escape_kind kind;
            qcc_status st = qcc_decode_escape(s, end, &pos, src, offset, diags,
                                              &v, &kind);
            if (st != QCC_OK) {
                return st;
            }
            if (narrow) {
                if (kind == QCC_ESCAPE_UCN) {
                    unsigned char buf[4];
                    size_t        k = qcc_utf8_encode(v, buf);
                    if (!sbuf_push_bytes(b, buf, k)) {
                        return QCC_ERR_OUT_OF_MEMORY;
                    }
                } else {
                    if (v > 0xFFu) {
                        st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                           "escape sequence out of range for "
                                           "string literal");
                        if (st != QCC_OK) {
                            return st;
                        }
                    }
                    if (!sbuf_push_unit(b, v & 0xFFu, 1)) {
                        return QCC_ERR_OUT_OF_MEMORY;
                    }
                }
            } else if (kind == QCC_ESCAPE_UCN) {
                if (!push_codepoint_wide(b, enc, v)) {
                    return QCC_ERR_OUT_OF_MEMORY;
                }
            } else {
                /* A numeric escape is a raw code unit (§6.4.4.4 ¶4). */
                uint32_t umax = qcc_encoding_unit_max(enc);
                if (v > umax) {
                    st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, 1,
                                       "escape sequence out of range for "
                                       "string literal");
                    if (st != QCC_OK) {
                        return st;
                    }
                    v &= umax;
                }
                if (!sbuf_push_unit(b, v, qcc_encoding_unit_size(enc))) {
                    return QCC_ERR_OUT_OF_MEMORY;
                }
            }
        } else if (narrow) {
            /* Narrow execution set is UTF-8: a source byte passes through. */
            if (!sbuf_push_unit(b, (unsigned char)s[pos], 1)) {
                return QCC_ERR_OUT_OF_MEMORY;
            }
            ++pos;
        } else {
            /* A wide piece decodes a source character to a code point. */
            uint32_t cp;
            if ((unsigned char)s[pos] < 0x80u) {
                cp = (unsigned char)s[pos];
                ++pos;
            } else {
                (void)qcc_utf8_decode(s, end, &pos, &cp);
            }
            if (!push_codepoint_wide(b, enc, cp)) {
                return QCC_ERR_OUT_OF_MEMORY;
            }
        }
    }
    return QCC_OK;
}

/* Resolve the combined encoding of the run (§6.4.5 ¶5): an unprefixed piece
   takes the others' prefix; a clash of two distinct non-empty prefixes is a
   diagnosed (implementation-defined-as-error) mix. */
static qcc_status reconcile_encoding(const qcc_ptok *pieces, size_t npieces,
                                     qcc_diag_sink *diags, qcc_char_encoding *out)
{
    qcc_char_encoding enc = QCC_ENC_PLAIN;
    for (size_t i = 0; i < npieces; ++i) {
        size_t            body;
        qcc_char_encoding penc = string_prefix(pieces[i].spelling,
                                               pieces[i].spelling_len, &body);
        if (penc == QCC_ENC_PLAIN) {
            continue;
        }
        if (enc == QCC_ENC_PLAIN) {
            enc = penc;
        } else if (enc != penc) {
            qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, pieces[i].source,
                                          pieces[i].offset, 1,
                                          "concatenation of string literals with "
                                          "different encoding prefixes");
            if (st != QCC_OK) {
                return st;
            }
            /* Keep the first non-empty prefix and continue best-effort. */
        }
    }
    *out = enc;
    return QCC_OK;
}

qcc_status qcc_eval_string(qcc_arena *arena, const qcc_ptok *pieces,
                           size_t npieces, qcc_diag_sink *diags,
                           const void **out_data, size_t *out_len,
                           qcc_char_encoding *out_encoding)
{
    if (arena == NULL || pieces == NULL || npieces == 0 || diags == NULL ||
        out_data == NULL || out_len == NULL || out_encoding == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_char_encoding enc;
    qcc_status        st = reconcile_encoding(pieces, npieces, diags, &enc);
    if (st != QCC_OK) {
        return st;
    }
    size_t unit = qcc_encoding_unit_size(enc);

    sbuf b = { NULL, 0, 0 };
    for (size_t i = 0; i < npieces; ++i) {
        st = decode_piece(&b, enc, pieces[i].spelling, pieces[i].spelling_len,
                          pieces[i].source, pieces[i].offset, diags);
        if (st != QCC_OK) {
            free(b.data);
            return st;
        }
    }

    size_t nunits = b.len / unit; /* Code units before the terminator. */
    if (!sbuf_push_unit(&b, 0u, unit)) {                  /* §6.4.5 ¶6. */
        free(b.data);
        return QCC_ERR_OUT_OF_MEMORY;
    }

    void *owned = qcc_arena_memdup(arena, b.data, b.len, unit);
    free(b.data);
    if (owned == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    *out_data     = owned;
    *out_len      = nunits;
    *out_encoding = enc;
    return QCC_OK;
}
