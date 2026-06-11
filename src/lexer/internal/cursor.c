/*
 * qcc — lexer internals: logical-character cursor, implementation.
 *
 * See cursor.h for the contracts. The functions are deliberately tiny and
 * branch-light: every byte the lexer ever looks at flows through qcc_lx_at,
 * so this file is the hot path of the whole front end.
 */
#include "lexer/internal/cursor.h"

int qcc_lx_is_space_not_nl(char c)
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

int qcc_lx_is_digit(char c) { return c >= '0' && c <= '9'; }

int qcc_lx_is_hex_digit(char c)
{
    return qcc_lx_is_digit(c) || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int qcc_lx_is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int qcc_lx_is_ident_cont(char c)
{
    return qcc_lx_is_ident_start(c) || qcc_lx_is_digit(c);
}

size_t qcc_lx_skip_splices(const qcc_source *src, size_t pos)
{
    for (;;) {
        if (src->data[pos] != '\\') {
            return pos;
        }
        size_t after = pos + 1;
        if (src->data[after] == '\n') {
            after += 1;
        } else if (src->data[after] == '\r' && src->data[after + 1] == '\n') {
            /* data[after + 1] is only read when data[after] == '\r', which
               implies after < size, so the read stays within the sentinel. */
            after += 2;
        } else {
            return pos; /* A real backslash, not a splice. */
        }
        pos = after; /* A splice may be followed by another splice. */
    }
}

char qcc_lx_at(const qcc_source *src, size_t pos, size_t *chpos, size_t *next)
{
    pos    = qcc_lx_skip_splices(src, pos);
    *chpos = pos;
    *next  = (pos < src->size) ? pos + 1 : pos;
    return src->data[pos];
}

size_t qcc_lx_spelling(const qcc_source *src, size_t offset, size_t length,
                       char *buf, size_t cap)
{
    size_t logical = 0;
    size_t pos     = offset;
    size_t end     = offset + length;

    while (pos < end) {
        pos = qcc_lx_skip_splices(src, pos);
        if (pos >= end) {
            break;
        }
        char c = src->data[pos];
        pos += 1;
        if (buf != NULL && logical < cap) {
            buf[logical] = c;
        }
        logical += 1;
    }
    return logical;
}
