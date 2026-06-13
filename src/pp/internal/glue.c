/*
 * qcc — preprocessor internals: the # and ## operators (implementation).
 *
 * See glue.h for the contract. Stringize builds the literal text in a byte
 * buffer and interns it; paste concatenates two spellings and re-lexes the
 * result with the real lexer over a throwaway source, so "is this one token?"
 * is answered by the same rules that tokenized the program (§6.4 maximal munch)
 * rather than a hand-rolled approximation.
 */
#include "pp/internal/glue.h"

#include <stdlib.h>
#include <string.h>

#include "diag/diag.h"
#include "lexer/lexer.h"
#include "source/source.h"

/* A small growable byte buffer for building synthesized spellings. */
typedef struct byte_buf {
    char  *data;
    size_t len;
    size_t cap;
} byte_buf;

static int bb_reserve(byte_buf *b, size_t extra)
{
    if (b->len > SIZE_MAX - extra) {
        return 0;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) {
        return 1;
    }
    size_t cap = (b->cap == 0) ? 64u : b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    char *grown = (char *)realloc(b->data, cap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap  = cap;
    return 1;
}

static int bb_putc(byte_buf *b, char c)
{
    if (!bb_reserve(b, 1)) {
        return 0;
    }
    b->data[b->len++] = c;
    return 1;
}

static int bb_put(byte_buf *b, const char *s, size_t n)
{
    if (!bb_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

static void bb_free(byte_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* Fill a synthesized token's common fields from an anchor (provenance source). */
static void synth_token(qcc_ptok *out, qcc_pp_token_kind kind, const char *spelling,
                        size_t len, const qcc_ptok *anchor)
{
    out->kind          = kind;
    out->punct         = (qcc_punct)0;
    out->spelling      = spelling;
    out->spelling_len  = len;
    out->source        = anchor->source;
    out->offset        = anchor->offset;
    out->line          = anchor->line;
    out->column        = anchor->column;
    out->leading_space = 0;
    out->at_line_start = 0;
    out->hideset       = NULL;
}

qcc_status qcc_pp_stringize(qcc_pp *pp, const qcc_ptok *toks, size_t count,
                            const qcc_ptok *anchor, qcc_ptok *out)
{
    if (pp == NULL || (toks == NULL && count != 0) || anchor == NULL ||
        out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    byte_buf b = { NULL, 0, 0 };
    int ok = bb_putc(&b, '"');

    for (size_t i = 0; ok && i < count; ++i) {
        const qcc_ptok *t = &toks[i];
        /* A single space stands in for any white space before this token; the
           first token contributes none (§6.10.3.2 ¶2: leading/trailing white
           space is deleted). */
        if (i != 0 && t->leading_space) {
            ok = bb_putc(&b, ' ');
        }

        int is_literal = (t->kind == QCC_PP_TOKEN_STRING_LIT ||
                          t->kind == QCC_PP_TOKEN_CHAR_CONST);
        for (size_t k = 0; ok && k < t->spelling_len; ++k) {
            char c = t->spelling[k];
            /* Inside a string/char literal, escape " and \ so the result is a
               valid spelling of the original (§6.10.3.2 ¶2). */
            if (is_literal && (c == '"' || c == '\\')) {
                ok = bb_putc(&b, '\\');
            }
            if (ok) {
                ok = bb_putc(&b, c);
            }
        }
    }
    if (ok) {
        ok = bb_putc(&b, '"');
    }
    if (!ok) {
        bb_free(&b);
        return QCC_ERR_OUT_OF_MEMORY;
    }

    const char *interned = qcc_pp_intern(pp, b.data, b.len);
    bb_free(&b);
    if (interned == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    synth_token(out, QCC_PP_TOKEN_STRING_LIT, interned, strlen(interned), anchor);
    return QCC_OK;
}

qcc_status qcc_pp_paste(qcc_pp *pp, const qcc_ptok *left, const qcc_ptok *right,
                        const qcc_ptok *anchor, qcc_ptok *out, int *out_ok)
{
    if (pp == NULL || left == NULL || right == NULL || anchor == NULL ||
        out == NULL || out_ok == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out_ok = 0;

    /* Build the concatenated spelling. */
    byte_buf b = { NULL, 0, 0 };
    int ok = bb_put(&b, left->spelling, left->spelling_len) &&
             bb_put(&b, right->spelling, right->spelling_len);
    if (!ok) {
        bb_free(&b);
        return QCC_ERR_OUT_OF_MEMORY;
    }

    /* Re-lex the concatenation. Lexer diagnostics go to a throwaway sink: a
       failed paste is reported as our own, clearer diagnostic below. */
    qcc_source    tmp;
    qcc_status    st = qcc_source_from_memory("<paste>", b.data, b.len, &tmp);
    size_t        concat_len = b.len;
    bb_free(&b);
    if (st != QCC_OK) {
        return st; /* OOM building the temp source. */
    }

    qcc_diag_sink scratch_diags;
    qcc_diag_sink_init(&scratch_diags);
    qcc_lexer lx;
    qcc_lexer_init(&lx, &tmp, &scratch_diags);

    qcc_pp_token first;
    st = qcc_lexer_next(&lx, &first);
    int single = 0;
    if (st == QCC_OK && first.kind != QCC_PP_TOKEN_EOF &&
        first.kind != QCC_PP_TOKEN_NEWLINE && first.offset == 0 &&
        first.length == concat_len) {
        /* The first token spans the whole concatenation; confirm nothing but
           the end-of-input follows (a second real token means two tokens). */
        qcc_pp_token second;
        st = qcc_lexer_next(&lx, &second);
        if (st == QCC_OK && (second.kind == QCC_PP_TOKEN_NEWLINE ||
                             second.kind == QCC_PP_TOKEN_EOF)) {
            single = 1;
        }
    }

    if (st != QCC_OK) {
        qcc_diag_sink_dispose(&scratch_diags);
        qcc_source_dispose(&tmp);
        return st;
    }

    if (single) {
        char buf_stack[64];
        char *buf  = buf_stack;
        char *heap = NULL;
        if (first.length > sizeof(buf_stack)) {
            heap = (char *)malloc(first.length);
            if (heap == NULL) {
                qcc_diag_sink_dispose(&scratch_diags);
                qcc_source_dispose(&tmp);
                return QCC_ERR_OUT_OF_MEMORY;
            }
            buf = heap;
        }
        size_t      len      = qcc_lexer_token_spelling(&tmp, &first, buf, first.length);
        const char *interned = qcc_pp_intern(pp, buf, len);
        free(heap);
        if (interned == NULL) {
            qcc_diag_sink_dispose(&scratch_diags);
            qcc_source_dispose(&tmp);
            return QCC_ERR_OUT_OF_MEMORY;
        }
        synth_token(out, first.kind, interned, len, anchor);
        out->punct = first.punct;
        *out_ok    = 1;
    } else {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, anchor->source, anchor->offset,
                      (anchor->spelling_len != 0) ? anchor->spelling_len : 1u,
                      "pasting \"%s\" and \"%s\" does not form a valid "
                      "preprocessing token", left->spelling, right->spelling);
        *out = *left; /* Recovery: keep the left operand. */
    }

    qcc_diag_sink_dispose(&scratch_diags);
    qcc_source_dispose(&tmp);
    return QCC_OK;
}
