/*
 * qcc — lexer internals: token-class scanners, implementation.
 *
 * Every scanner walks physical offsets with qcc_lx_at (so phase-2 splices
 * are invisible to it) and reports an extent; see scan.h for contracts and
 * the standard clauses each one implements.
 */
#include "lexer/internal/scan.h"

#include <string.h>

#include "lexer/internal/cursor.h"

qcc_status qcc_lx_warn_trigraph(const qcc_source *src, qcc_diag_sink *diags,
                                size_t qmark_pos)
{
    size_t p1, n1, p2, n2, p3, n3;

    if (qcc_lx_at(src, qmark_pos, &p1, &n1) != '?') {
        return QCC_OK;
    }
    if (qcc_lx_at(src, n1, &p2, &n2) != '?') {
        return QCC_OK;
    }
    char third = qcc_lx_at(src, n2, &p3, &n3);
    if (third == '\0' || strchr("=(/)'<!>-", third) == NULL) {
        return QCC_OK;
    }
    return qcc_diag_emit(diags, QCC_DIAG_WARNING, src, qmark_pos,
                         n3 - qmark_pos,
                         "trigraph '?\?%c' ignored (qcc does not translate "
                         "trigraphs)", third);
}

/*
 * Scan a universal-character-name continuation: at entry `bs_next` is the
 * offset just past a '\'. On a syntactically valid UCN (§6.4.3: \u + 4 hex
 * digits, or \U + 8) sets *out_end one past it and returns 1; otherwise 0
 * (the caller decides what the backslash is). Whether the designated
 * character is *allowed* in an identifier is a phase-7 question.
 */
static int scan_ucn(const qcc_source *src, size_t bs_next, size_t *out_end)
{
    size_t chpos, next;
    char   u = qcc_lx_at(src, bs_next, &chpos, &next);
    int    want;

    if (u == 'u') {
        want = 4;
    } else if (u == 'U') {
        want = 8;
    } else {
        return 0;
    }

    size_t p = next;
    for (int i = 0; i < want; ++i) {
        char h = qcc_lx_at(src, p, &chpos, &next);
        if (!qcc_lx_is_hex_digit(h)) {
            return 0;
        }
        p = next;
    }
    *out_end = p;
    return 1;
}

qcc_status qcc_lx_scan_identifier(const qcc_source *src, qcc_diag_sink *diags,
                                  size_t start, qcc_lx_scan *out)
{
    size_t p = start;

    out->punct = (qcc_punct)0;

    for (;;) {
        size_t chpos, next;
        char   c = qcc_lx_at(src, p, &chpos, &next);

        if (qcc_lx_is_ident_cont(c)) {
            p = next;
            continue;
        }
        if (c == '\\') {
            size_t ucn_end;
            if (scan_ucn(src, next, &ucn_end)) {
                p = ucn_end;
                continue;
            }
            if (p == start) {
                /*
                 * A backslash that opens no UCN cannot begin an identifier
                 * after all: it is a stray character — an "other" pp-token
                 * (§6.4 ¶1), diagnosed because it can never survive phase 7.
                 */
                qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src,
                                              chpos, 1,
                                              "stray '\\' in program");
                if (st != QCC_OK) {
                    return st;
                }
                out->kind = QCC_PP_TOKEN_OTHER;
                out->end  = next;
                return QCC_OK;
            }
        }
        break;
    }

    /* Literal prefix? Read the logical spelling (at most "u8" can match) and
       peek at the character after the identifier. */
    size_t qpos, qnext;
    char   q = qcc_lx_at(src, p, &qpos, &qnext);
    if (q == '\'' || q == '"') {
        char   buf[3];
        size_t n = qcc_lx_spelling(src, start, p - start, buf, sizeof(buf));

        int is_prefix =
            (n == 1 && (buf[0] == 'L' || buf[0] == 'u' || buf[0] == 'U')) ||
            (n == 2 && buf[0] == 'u' && buf[1] == '8' && q == '"');
        /* u8 prefixes strings only (§6.4.5); u8'c' is not a C11 token. */

        if (is_prefix) {
            return qcc_lx_scan_literal(src, diags, qpos, q, out);
        }
    }

    out->kind = QCC_PP_TOKEN_IDENTIFIER;
    out->end  = p;
    return QCC_OK;
}

qcc_status qcc_lx_scan_pp_number(const qcc_source *src, size_t start,
                                 qcc_lx_scan *out)
{
    size_t chpos, next;

    (void)qcc_lx_at(src, start, &chpos, &next); /* Consume the first char. */
    size_t p = next;

    for (;;) {
        char c = qcc_lx_at(src, p, &chpos, &next);

        if (c == 'e' || c == 'E' || c == 'p' || c == 'P') {
            /* The exponent letter, and a sign directly after it if any —
               the ONLY place a sign continues a pp-number (§6.4.8). */
            size_t spos, snext;
            char   s = qcc_lx_at(src, next, &spos, &snext);
            p = (s == '+' || s == '-') ? snext : next;
            continue;
        }
        if (qcc_lx_is_ident_cont(c) || c == '.') {
            p = next;
            continue;
        }
        if (c == '\\') {
            size_t ucn_end;
            if (scan_ucn(src, next, &ucn_end)) {
                p = ucn_end;
                continue;
            }
        }
        break;
    }

    out->kind  = QCC_PP_TOKEN_PP_NUMBER;
    out->punct = (qcc_punct)0;
    out->end   = p;
    return QCC_OK;
}

qcc_status qcc_lx_scan_literal(const qcc_source *src, qcc_diag_sink *diags,
                               size_t quote_pos, char quote,
                               qcc_lx_scan *out)
{
    size_t p          = quote_pos + 1;
    size_t end        = 0;
    int    terminated = 0;

    for (;;) {
        size_t chpos, next;
        char   c = qcc_lx_at(src, p, &chpos, &next);

        if (c == '\0' && chpos >= src->size) {
            end = chpos;
            break;
        }
        if (c == '\n') {
            end = chpos; /* Token stops before the newline. */
            break;
        }
        if (c == quote) {
            terminated = 1;
            end        = next;
            break;
        }
        if (c == '\\') {
            /* Escape sequence: the next logical character is part of it,
               whatever it is. (A backslash directly before a newline never
               reaches here — that is a phase-2 splice.) */
            size_t epos, enext;
            char   e = qcc_lx_at(src, next, &epos, &enext);
            if (e == '\0' && epos >= src->size) {
                end = epos;
                break;
            }
            p = enext;
            continue;
        }
        if (c == '?') {
            qcc_status st = qcc_lx_warn_trigraph(src, diags, chpos);
            if (st != QCC_OK) {
                return st;
            }
        }
        p = next;
    }

    if (!terminated) {
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, quote_pos, 1,
                                      "missing terminating %c character",
                                      quote);
        if (st != QCC_OK) {
            return st;
        }
    }

    out->kind  = (quote == '\'') ? QCC_PP_TOKEN_CHAR_CONST
                                 : QCC_PP_TOKEN_STRING_LIT;
    out->punct = (qcc_punct)0;
    out->end   = end;
    return QCC_OK;
}

qcc_status qcc_lx_scan_header_name(const qcc_source *src,
                                   qcc_diag_sink *diags, size_t start,
                                   char open, qcc_lx_scan *out)
{
    char   close      = (open == '<') ? '>' : '"';
    size_t p          = start + 1;
    size_t end        = 0;
    int    terminated = 0;

    for (;;) {
        size_t chpos, next;
        char   c = qcc_lx_at(src, p, &chpos, &next);

        if ((c == '\0' && chpos >= src->size) || c == '\n') {
            end = chpos;
            break;
        }
        if (c == close) {
            terminated = 1;
            end        = next;
            break;
        }
        p = next;
    }

    if (!terminated) {
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, start, 1,
                                      "missing terminating %c in header name",
                                      close);
        if (st != QCC_OK) {
            return st;
        }
    }

    out->kind  = QCC_PP_TOKEN_HEADER_NAME;
    out->punct = (qcc_punct)0;
    out->end   = end;
    return QCC_OK;
}
