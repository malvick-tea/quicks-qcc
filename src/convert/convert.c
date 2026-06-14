/*
 * qcc — convert: preprocessing tokens to tokens (implementation).
 *
 * See convert.h and ADR-0017. This is Unit A: reclassification. Each
 * preprocessing token becomes a token of the corresponding §6.4 category, with
 * two real decisions — an identifier that spells a keyword becomes a keyword
 * (§6.4.1 ¶2), and a pp-number is classified as an integer or floating constant
 * by its §6.4.8 shape (§6.4.4) — and stray tokens that §6.4 ¶3 forbids in a
 * program are diagnosed. Constant values and string concatenation land in later
 * units; a constant token here carries its source lexeme.
 */
#include "convert/convert.h"

#include <stdlib.h>

#include "convert/internal/intconst.h"

/* The token list. */

void qcc_token_list_init(qcc_token_list *list)
{
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qcc_token_list_dispose(qcc_token_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qcc_token_list_clear(qcc_token_list *list)
{
    if (list != NULL) {
        list->count = 0;
    }
}

qcc_status qcc_token_list_push(qcc_token_list *list, const qcc_token *tok)
{
    if (list == NULL || tok == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (list->count == list->capacity) {
        size_t     ncap  = (list->capacity == 0) ? 16u : list->capacity * 2u;
        qcc_token *grown = (qcc_token *)realloc(list->items, ncap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        list->items    = grown;
        list->capacity = ncap;
    }
    list->items[list->count++] = *tok;
    return QCC_OK;
}

/* The converter. */

qcc_status qcc_convert_init(qcc_convert *cv, qcc_diag_sink *diags)
{
    if (cv == NULL || diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&cv->arena, 0);
    cv->diags = diags;

    qcc_status st = qcc_intern_init(&cv->interner, &cv->arena);
    if (st != QCC_OK) {
        qcc_arena_dispose(&cv->arena);
        return st;
    }
    return QCC_OK;
}

void qcc_convert_dispose(qcc_convert *cv)
{
    if (cv == NULL) {
        return;
    }
    qcc_intern_dispose(&cv->interner);
    qcc_arena_dispose(&cv->arena);
    cv->diags = NULL;
}

/* Private helpers. */

/* Underline width for a token in diagnostics (at least one column). */
static size_t tok_span(const qcc_ptok *t)
{
    return (t->spelling_len != 0) ? t->spelling_len : 1u;
}

/*
 * A pp-number (§6.4.8) is a floating constant (§6.4.4.2) iff its *shape* shows a
 * fraction or an exponent: a '.', a decimal 'e'/'E' exponent, or — for a
 * hexadecimal number (0x/0X) — a binary 'p'/'P' exponent. Otherwise it is an
 * integer constant (§6.4.4.1). The shape decides the category independently of
 * the value (which a later unit computes); e.g. 0x1p3 is floating, 0xE5 is not.
 */
static qcc_token_kind classify_ppnumber(const char *s, size_t n)
{
    int is_hex = (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '.') {
            return QCC_TOKEN_FLOATING;
        }
        if (is_hex) {
            if (c == 'p' || c == 'P') {
                return QCC_TOKEN_FLOATING;
            }
        } else if (c == 'e' || c == 'E') {
            return QCC_TOKEN_FLOATING;
        }
    }
    return QCC_TOKEN_INTEGER;
}

/* Fill a token from a preprocessing token, interning its spelling through the
   converter's own interner. Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY. */
static qcc_status fill_token(qcc_convert *cv, const qcc_ptok *in,
                             qcc_token_kind kind, qcc_token *out)
{
    const char *sp = qcc_intern_bytes(&cv->interner, in->spelling,
                                      in->spelling_len);
    if (sp == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    out->kind          = kind;
    out->keyword       = QCC_KW_NONE;
    out->punct         = in->punct;
    out->spelling      = sp;
    out->spelling_len  = in->spelling_len;
    out->source        = in->source;
    out->offset        = in->offset;
    out->line          = in->line;
    out->column        = in->column;
    out->leading_space = in->leading_space;
    out->int_value     = 0;
    out->int_type      = QCC_INT_INT;
    return QCC_OK;
}

/* Build the terminating EOF token from the input's EOF (no interning). */
static qcc_token eof_token(const qcc_ptok *in)
{
    qcc_token t;
    t.kind          = QCC_TOKEN_EOF;
    t.keyword       = QCC_KW_NONE;
    t.punct         = (qcc_punct)0;
    t.spelling      = "";
    t.spelling_len  = 0;
    t.source        = (in != NULL) ? in->source : NULL;
    t.offset        = (in != NULL) ? in->offset : 0;
    t.line          = (in != NULL) ? in->line : 0;
    t.column        = (in != NULL) ? in->column : 0;
    t.leading_space = 0;
    t.int_value     = 0;
    t.int_type      = QCC_INT_INT;
    return t;
}

/* Public interface. */

qcc_status qcc_convert_run(qcc_convert *cv, const qcc_ptok_list *in,
                           qcc_token_list *out)
{
    if (cv == NULL || in == NULL || out == NULL || cv->diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < in->count; ++i) {
        const qcc_ptok *t = &in->items[i];

        if (t->kind == QCC_PP_TOKEN_EOF) {
            qcc_token eof = eof_token(t);
            return qcc_token_list_push(out, &eof); /* EOF terminates the stream. */
        }

        qcc_token_kind kind = QCC_TOKEN_PUNCT;
        qcc_keyword    kw   = QCC_KW_NONE;

        switch (t->kind) {
        case QCC_PP_TOKEN_IDENTIFIER:
            kw   = qcc_keyword_lookup(t->spelling, t->spelling_len);
            kind = (kw != QCC_KW_NONE) ? QCC_TOKEN_KEYWORD : QCC_TOKEN_IDENTIFIER;
            break;
        case QCC_PP_TOKEN_PP_NUMBER:
            kind = classify_ppnumber(t->spelling, t->spelling_len);
            break;
        case QCC_PP_TOKEN_CHAR_CONST:
            kind = QCC_TOKEN_CHAR;
            break;
        case QCC_PP_TOKEN_STRING_LIT:
            kind = QCC_TOKEN_STRING;
            break;
        case QCC_PP_TOKEN_PUNCT:
            kind = QCC_TOKEN_PUNCT;
            break;
        case QCC_PP_TOKEN_HEADER_NAME:
            /* A header-name exists only inside #include (§6.4.7); one surviving
               to phase 7 is a stray token (§6.4 ¶3). */
            qcc_diag_emit(cv->diags, QCC_DIAG_ERROR, t->source, t->offset,
                          tok_span(t), "stray header-name in program");
            continue;
        case QCC_PP_TOKEN_OTHER:
            /* §6.4 ¶3: a non-white-space character that is not a token makes the
               program ill-formed at this phase. */
            qcc_diag_emit(cv->diags, QCC_DIAG_ERROR, t->source, t->offset,
                          tok_span(t), "stray '%.*s' in program",
                          (int)t->spelling_len, t->spelling);
            continue;
        case QCC_PP_TOKEN_NEWLINE:
        case QCC_PP_TOKEN_EOF:
        default:
            /* Newlines do not occur in phase-4 output; ignore defensively. */
            continue;
        }

        qcc_token tok;
        qcc_status st = fill_token(cv, t, kind, &tok);
        if (st != QCC_OK) {
            return st;
        }
        tok.keyword = kw; /* QCC_KW_NONE except for a keyword token. */

        /* Evaluate an integer constant's value and type (§6.4.4.1). */
        if (kind == QCC_TOKEN_INTEGER) {
            st = qcc_eval_integer(tok.spelling, tok.spelling_len, t->source,
                                  t->offset, cv->diags, &tok.int_value,
                                  &tok.int_type);
            if (st != QCC_OK) {
                return st;
            }
        }

        st = qcc_token_list_push(out, &tok);
        if (st != QCC_OK) {
            return st;
        }
    }

    /* The input was not EOF-terminated (it always is from qcc_pp_run); be safe. */
    qcc_token eof = eof_token(NULL);
    return qcc_token_list_push(out, &eof);
}
