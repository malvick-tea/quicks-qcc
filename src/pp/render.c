/*
 * qcc — render a preprocessing-token stream back to text (implementation).
 *
 * See render.h. The output re-lexes to the same token sequence: lines are
 * reconstructed from at_line_start, inter-token spacing from leading_space, and
 * a space is forced wherever two adjacent tokens would otherwise paste under
 * §6.4 maximal munch. The text is built in a growable heap buffer; the caller
 * frees it.
 */
#include "pp/render.h"

#include <stdlib.h>
#include <string.h>

#include "token/token.h"

/* A growable byte buffer over the seed allocator (ADR-0009). It always keeps
   room for a terminating NUL, so render can NUL-terminate without a final grow. */
typedef struct rbuf {
    char  *data;
    size_t len;
    size_t cap;
} rbuf;

/* Ensure at least `extra` more bytes plus a NUL fit. Returns 0 on OOM. */
static int rbuf_reserve(rbuf *b, size_t extra)
{
    if (b->data != NULL && b->len + extra + 1 <= b->cap) {
        return 1;
    }
    size_t need = b->len + extra + 1;
    size_t ncap = (b->cap == 0) ? 256u : b->cap;
    while (ncap < need) {
        ncap *= 2u;
    }
    char *grown = (char *)realloc(b->data, ncap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap  = ncap;
    return 1;
}

static int rbuf_putc(rbuf *b, char c)
{
    if (!rbuf_reserve(b, 1)) {
        return 0;
    }
    b->data[b->len++] = c;
    return 1;
}

static int rbuf_put(rbuf *b, const char *s, size_t n)
{
    if (n == 0) {
        return 1;
    }
    if (!rbuf_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

/* An identifier/pp-number continuation character (§6.4.2.1, §6.4.8). */
static int is_ident_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * Would emitting `prev` immediately followed by `cur`, with no separating space,
 * re-lex as anything other than those two tokens? Conservative — when unsure it
 * returns 1 (forcing a space is always safe). It guards the maximal-munch
 * hazards (§6.4 ¶4): identifier/number runs merging, two punctuators forming a
 * longer punctuator, a literal prefix gluing onto a preceding identifier, and a
 * pp-number swallowing a following '.'/'+'/'-' (or a leading '.' a number).
 */
static int needs_space(const qcc_ptok *prev, const qcc_ptok *cur)
{
    if (prev->spelling_len == 0 || cur->spelling_len == 0) {
        return 0;
    }
    char a = prev->spelling[prev->spelling_len - 1];
    char b = cur->spelling[0];

    if (is_ident_char(a) && is_ident_char(b)) {
        return 1;
    }
    if (prev->kind == QCC_PP_TOKEN_PUNCT && cur->kind == QCC_PP_TOKEN_PUNCT) {
        return 1;
    }
    if ((cur->kind == QCC_PP_TOKEN_STRING_LIT ||
         cur->kind == QCC_PP_TOKEN_CHAR_CONST) && is_ident_char(a)) {
        return 1;
    }
    if (prev->kind == QCC_PP_TOKEN_PP_NUMBER &&
        (b == '.' || b == '+' || b == '-')) {
        return 1;
    }
    if (a == '.' && cur->kind == QCC_PP_TOKEN_PP_NUMBER) {
        return 1;
    }
    return 0;
}

qcc_status qcc_pp_render(const qcc_ptok_list *toks, char **out_text,
                         size_t *out_len)
{
    if (toks == NULL || out_text == NULL || out_len == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out_text = NULL;
    *out_len  = 0;

    rbuf            b       = { NULL, 0, 0 };
    const qcc_ptok *prev    = NULL;
    int             started = 0;

    for (size_t i = 0; i < toks->count; ++i) {
        const qcc_ptok *t = &toks->items[i];
        if (t->kind == QCC_PP_TOKEN_EOF) {
            break; /* The terminator is not emitted. */
        }

        if (!started) {
            started = 1; /* No separator before the very first token. */
        } else if (t->at_line_start) {
            if (!rbuf_putc(&b, '\n')) {
                goto oom;
            }
        } else if (t->leading_space || needs_space(prev, t)) {
            if (!rbuf_putc(&b, ' ')) {
                goto oom;
            }
        }

        if (!rbuf_put(&b, t->spelling, t->spelling_len)) {
            goto oom;
        }
        prev = t;
    }

    if (started && !rbuf_putc(&b, '\n')) {
        goto oom; /* A non-empty translation unit ends with a newline. */
    }

    if (b.data == NULL) {
        b.data = (char *)malloc(1); /* Empty input: an empty string. */
        if (b.data == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
    }
    b.data[b.len] = '\0'; /* Room reserved by every grow. */
    *out_text     = b.data;
    *out_len      = b.len;
    return QCC_OK;

oom:
    free(b.data);
    return QCC_ERR_OUT_OF_MEMORY;
}
