/*
 * qcc — the lexer: translation phases 1-3 (ISO C11 §5.1.1.2)
 *
 * Responsibility
 * Turn the physical bytes of one qcc_source into the preprocessing-token
 * stream of §6.4, performing exactly the first three translation phases:
 *
 *   phase 1  physical characters. We accept the bytes as-is; trigraph
 *            sequences are DIAGNOSED (warning) and left untranslated — a
 *            deliberate, documented deviation recorded in ADR-0013 (C23
 *            removed trigraphs; no Quicks source uses them).
 *   phase 2  line splicing: each backslash immediately followed by a newline
 *            is deleted. This happens *everywhere* — inside identifiers,
 *            pp-numbers, string literals, even between the two '<' of "<<" —
 *            so the lexer reads through a splice-skipping accessor rather
 *            than indexing bytes directly.
 *   phase 3  decomposition into preprocessing tokens, with each comment
 *            replaced by one space. Maximal munch governs (§6.4 ¶4): the next
 *            pp-token is the longest sequence of characters that could
 *            constitute one.
 *
 * What the lexer does NOT do (and why)
 *   - No macro expansion, no directives: that is phase 4, the preprocessor's
 *     job; the lexer just marks newline tokens and line-start/leading-space
 *     facts so the preprocessor can be line-oriented (§6.10 ¶2).
 *   - No constant evaluation, no escape validation: phase 7 (the convert
 *     stage) turns pp-numbers/literals into typed values per §6.4.4; the
 *     lexer only fixes token *extents*.
 *
 * Error philosophy (error-handling.md)
 *   Lexical problems (unterminated comment/literal, malformed universal
 *   character name) are reported as diagnostics on the sink and lexing
 *   continues — users get every problem in one run. qcc_lexer_next itself
 *   fails only on hard faults (out of memory while recording a diagnostic).
 */
#ifndef QCC_LEXER_LEXER_H
#define QCC_LEXER_LEXER_H

#include <stddef.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Lexer state over one source. Borrowed pointers: the source and sink must
 * outlive the lexer. Treat the fields as private; use the functions below.
 */
typedef struct qcc_lexer {
    const qcc_source *source;        /* Borrowed input.                         */
    qcc_diag_sink    *diags;         /* Borrowed diagnostics sink.              */
    size_t            pos;           /* Current physical byte offset.           */
    unsigned          at_line_start : 1; /* Next token is first on its line.    */
    unsigned          header_mode   : 1; /* §6.4.7 header-name mode (below).    */
    unsigned          eof_newline_done : 1; /* Synthetic final newline emitted. */
} qcc_lexer;

/* Bind a lexer to a source and a diagnostics sink (both must be non-NULL and
   outlive the lexer). Lexing starts at the first byte. */
void qcc_lexer_init(qcc_lexer *lexer, const qcc_source *source,
                    qcc_diag_sink *diags);

/*
 * Produce the next preprocessing token into *out.
 *
 * The stream always ends with QCC_PP_TOKEN_EOF (returned again on every call
 * after the end — safe to over-read). A source whose last line lacks a final
 * newline gets one zero-length QCC_PP_TOKEN_NEWLINE synthesized before EOF,
 * so every logical line is newline-terminated and the preprocessor needs no
 * special end-of-file case (§5.1.1.2 phase 2 requires the newline; we repair
 * instead of rejecting and document it here).
 *
 * Returns QCC_OK (including for tokens that carry diagnosed errors), or
 * QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY on hard faults.
 */
qcc_status qcc_lexer_next(qcc_lexer *lexer, qcc_pp_token *out);

/*
 * Switch header-name mode on/off (§6.4.7). While ON, a token starting with
 * '<' is lexed as a header-name up to '>' (and "..." likewise up to '"')
 * instead of as punctuators/string — header-names exist ONLY between the
 * #include directive's name and the newline, and only the preprocessor knows
 * when that is, so the mode is explicit. An unterminated header-name (newline
 * or EOF first) is diagnosed and the token ends at the line end.
 */
void qcc_lexer_set_header_mode(qcc_lexer *lexer, int on);

/*
 * Copy a token's *logical* spelling — its source bytes with every line splice
 * (backslash-newline) removed — into buf. Tokens may carry interior splices
 * (see token.h), so spans must be read through this, never memcpy'd.
 *
 * Copies at most `cap` bytes (no NUL is appended) and returns the full
 * logical length, which is <= tok->length: a buffer of tok->length bytes is
 * always large enough.
 */
size_t qcc_lexer_token_spelling(const qcc_source *source,
                                const qcc_pp_token *tok,
                                char *buf, size_t cap);

#endif /* QCC_LEXER_LEXER_H */
