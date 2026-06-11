/*
 * qcc — the lexer: the public driver.
 *
 * Layout of the module (ADR-0008: private parts under internal/)
 *   internal/cursor — phases 1-2: the splice-skipping logical reader and the
 *                     character classes. The only place that touches bytes.
 *   internal/scan   — phase 3 scanners: identifier/UCN (with literal-prefix
 *                     morphing), pp-number, char/string bodies, header-names.
 *   internal/punct  — phase 3 scanner: maximal-munch punctuators + "other".
 *   lexer.c (here)  — whitespace/comment/newline handling, end-of-input
 *                     repair, dispatch to the scanners, and materializing
 *                     their extents into qcc_pp_token values.
 *
 * Scanners are pure (offset in, extent out); ALL lexer-state mutation —
 * cursor movement, the line-start flag — happens in make_token below, so
 * there is exactly one place where a token "takes effect".
 */
#include "lexer/lexer.h"

#include "lexer/internal/cursor.h"
#include "lexer/internal/punct.h"
#include "lexer/internal/scan.h"

/*
 * Fill *out and advance the lexer. `end` is one past the token's last
 * physical byte; the cursor moves there. The token inherits the pending
 * line-start flag and the new flag is 1 exactly after a newline token, which
 * is what "first pp-token on its logical line" (§6.10 ¶2) means here.
 */
static qcc_status make_token(qcc_lexer *lx, qcc_pp_token *out,
                             qcc_pp_token_kind kind, size_t start, size_t end,
                             unsigned leading_space, int is_newline)
{
    uint32_t line   = 0;
    uint32_t column = 0;
    (void)qcc_source_location(lx->source, start, &line, &column);

    out->kind          = kind;
    out->punct         = (qcc_punct)0;
    out->offset        = start;
    out->length        = end - start;
    out->line          = line;
    out->column        = column;
    out->leading_space = leading_space ? 1u : 0u;
    out->at_line_start = lx->at_line_start;

    lx->at_line_start = is_newline ? 1u : 0u;
    lx->pos           = end;
    return QCC_OK;
}

/* Public interface. */

void qcc_lexer_init(qcc_lexer *lexer, const qcc_source *source,
                    qcc_diag_sink *diags)
{
    if (lexer == NULL) {
        return;
    }
    lexer->source           = source;
    lexer->diags            = diags;
    lexer->pos              = 0;
    lexer->at_line_start    = 1; /* Offset 0 is the start of line 1. */
    lexer->header_mode      = 0;
    lexer->eof_newline_done = 0;
}

void qcc_lexer_set_header_mode(qcc_lexer *lexer, int on)
{
    if (lexer != NULL) {
        lexer->header_mode = on ? 1u : 0u;
    }
}

qcc_status qcc_lexer_next(qcc_lexer *lexer, qcc_pp_token *out)
{
    if (lexer == NULL || out == NULL || lexer->source == NULL ||
        lexer->diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    const qcc_source *src = lexer->source;
    unsigned leading_space = 0;
    size_t   chpos, next;
    char     c;

    /*
     * Whitespace, comments, newlines (phase 3). Block comments swallow their
     * interior newlines on purpose: the comment was replaced by ONE space, so
     * a directive spanning lines through a comment stays a single logical
     * line, exactly as §5.1.1.2 prescribes.
     */
    for (;;) {
        c = qcc_lx_at(src, lexer->pos, &chpos, &next);

        if (c == '\0' && chpos >= src->size) {
            /* End of input. Repair a missing final newline once (see lexer.h),
               then report EOF forever. */
            if (!lexer->at_line_start && !lexer->eof_newline_done) {
                lexer->eof_newline_done = 1;
                return make_token(lexer, out, QCC_PP_TOKEN_NEWLINE, src->size,
                                  src->size, leading_space, 1);
            }
            return make_token(lexer, out, QCC_PP_TOKEN_EOF, src->size,
                              src->size, leading_space, 0);
        }
        if (c == '\n') {
            return make_token(lexer, out, QCC_PP_TOKEN_NEWLINE, chpos, next,
                              leading_space, 1);
        }
        if (qcc_lx_is_space_not_nl(c)) {
            leading_space = 1;
            lexer->pos    = next;
            continue;
        }
        if (c == '/') {
            size_t c2pos, c2next;
            char   c2 = qcc_lx_at(src, next, &c2pos, &c2next);

            if (c2 == '/') {
                /* Line comment: up to (not including) the newline/EOF. */
                size_t p = c2next;
                for (;;) {
                    char cc = qcc_lx_at(src, p, &c2pos, &c2next);
                    if (cc == '\n' || (cc == '\0' && c2pos >= src->size)) {
                        break;
                    }
                    p = c2next;
                }
                lexer->pos    = p;
                leading_space = 1;
                continue;
            }
            if (c2 == '*') {
                /* Block comment: up to the first * / pair (§6.4.9 ¶1: block
                   comments do not nest). Unterminated is diagnosed at the
                   opener so the user sees where it began. */
                size_t p      = c2next;
                int    closed = 0;
                for (;;) {
                    char cc = qcc_lx_at(src, p, &c2pos, &c2next);
                    if (cc == '\0' && c2pos >= src->size) {
                        break;
                    }
                    if (cc == '*') {
                        size_t c3pos, c3next;
                        char   c3 = qcc_lx_at(src, c2next, &c3pos, &c3next);
                        if (c3 == '/') {
                            p      = c3next;
                            closed = 1;
                            break;
                        }
                    }
                    p = c2next;
                }
                if (!closed) {
                    qcc_status st = qcc_diag_emit(lexer->diags, QCC_DIAG_ERROR,
                                                  src, chpos, 2,
                                                  "unterminated /* comment");
                    if (st != QCC_OK) {
                        return st;
                    }
                    lexer->pos = c2pos; /* At EOF; next call reports it. */
                } else {
                    lexer->pos = p;
                }
                leading_space = 1;
                continue;
            }
            break; /* A '/' token (or '/=') — fall through to dispatch. */
        }
        break;
    }

    /* Dispatch on the first significant character (maximal munch, §6.4 ¶4).
       Each scanner returns an extent; make_token materializes it. */
    size_t      start = chpos;
    qcc_lx_scan scan;
    qcc_status  st;

    if (lexer->header_mode && (c == '<' || c == '"')) {
        /* Header-name mode (§6.4.7): switched on by the preprocessor right
           after "#include"; only < and " open a header-name. */
        st = qcc_lx_scan_header_name(src, lexer->diags, start, c, &scan);
    } else if (qcc_lx_is_ident_start(c) || c == '\\') {
        st = qcc_lx_scan_identifier(src, lexer->diags, start, &scan);
    } else if (qcc_lx_is_digit(c)) {
        st = qcc_lx_scan_pp_number(src, start, &scan);
    } else if (c == '.') {
        /* '.' starts a pp-number iff a digit follows (§6.4.8). */
        size_t dpos, dnext;
        char   d = qcc_lx_at(src, next, &dpos, &dnext);
        if (qcc_lx_is_digit(d)) {
            st = qcc_lx_scan_pp_number(src, start, &scan);
        } else {
            st = qcc_lx_scan_punct_or_other(src, lexer->diags, start, &scan);
        }
    } else if (c == '\'' || c == '"') {
        st = qcc_lx_scan_literal(src, lexer->diags, start, c, &scan);
    } else {
        st = qcc_lx_scan_punct_or_other(src, lexer->diags, start, &scan);
    }

    if (st != QCC_OK) {
        return st;
    }

    st = make_token(lexer, out, scan.kind, start, scan.end, leading_space, 0);
    out->punct = scan.punct;
    return st;
}

size_t qcc_lexer_token_spelling(const qcc_source *source,
                                const qcc_pp_token *tok, char *buf, size_t cap)
{
    if (source == NULL || tok == NULL) {
        return 0;
    }
    return qcc_lx_spelling(source, tok->offset, tok->length, buf, cap);
}
