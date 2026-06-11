/*
 * qcc — lexer internals: token-class scanners (phase 3, §6.4).
 *
 * Responsibility
 * One scanner per preprocessing-token class with internal structure:
 * identifier (with UCNs and literal-prefix morphing), pp-number, character-
 * constant/string-literal bodies, and header-names. Scanners are pure with
 * respect to lexer state: they take a start offset, read through the cursor,
 * emit diagnostics if needed, and report the token's class and physical end
 * in a qcc_lx_scan — the driver (lexer.c) materializes the token and moves
 * the cursor. That seam keeps every scanner unit-reasoned: extent in, extent
 * out, no hidden mutation.
 *
 * Internal header (ADR-0008): only lexer/ files may include it.
 */
#ifndef QCC_LEXER_INTERNAL_SCAN_H
#define QCC_LEXER_INTERNAL_SCAN_H

#include <stddef.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/* A scanner's verdict: what the token is and where it physically ends. */
typedef struct qcc_lx_scan {
    qcc_pp_token_kind kind;
    qcc_punct         punct; /* Valid iff kind == QCC_PP_TOKEN_PUNCT.        */
    size_t            end;   /* One past the token's last physical byte.     */
} qcc_lx_scan;

/*
 * Phase 1 deviation, diagnosed: if a trigraph sequence (§5.2.1.1) starts at
 * qmark_pos, warn that it is NOT translated (ADR-0013; C23 removed trigraphs
 * and translating ??! inside a string would silently change its bytes).
 * Shared by the punctuator scanner ('?') and the literal scanner (bodies).
 */
qcc_status qcc_lx_warn_trigraph(const qcc_source *src, qcc_diag_sink *diags,
                                size_t qmark_pos);

/*
 * identifier (§6.4.2.1): nondigit, then nondigit/digit/UCN (§6.4.3). If the
 * whole identifier is a literal prefix (L/u/U, or u8 before '"' only) and a
 * quote follows immediately, the scan morphs into that literal — maximal
 * munch (§6.4 ¶4) makes L'a' ONE character-constant while u8x"s" stays
 * identifier + string. May be entered at a '\' that opens a UCN; a stray
 * backslash is diagnosed and reported as a one-character "other" token.
 */
qcc_status qcc_lx_scan_identifier(const qcc_source *src, qcc_diag_sink *diags,
                                  size_t start, qcc_lx_scan *out);

/*
 * pp-number (§6.4.8): digit or '.'digit start; continues with digits,
 * identifier characters (incl. UCNs), '.', and a sign — but ONLY right after
 * e/E/p/P. Hence "0x1p-3" and ".5e+2" are one pp-number each, and so is the
 * invalid-on-conversion "0xE+1". Never diagnoses: meaning is phase 7's job.
 */
qcc_status qcc_lx_scan_pp_number(const qcc_source *src, size_t start,
                                 qcc_lx_scan *out);

/*
 * character-constant (§6.4.4.4) / string-literal (§6.4.5) body, scanned from
 * the opening quote at `quote_pos`. The token's *start* (which may sit
 * earlier, at a L/u/U/u8 prefix) is the driver's concern — a scanner only
 * reports the end. Escapes are consumed blindly ('\' + one logical
 * character): \" must not close the literal, but escape *validity* is phase
 * 7's question. A newline/EOF inside is diagnosed; the token ends there.
 */
qcc_status qcc_lx_scan_literal(const qcc_source *src, qcc_diag_sink *diags,
                               size_t quote_pos, char quote,
                               qcc_lx_scan *out);

/*
 * header-name (§6.4.7): <h-chars> or "q-chars", entered only in header-name
 * mode. The closing character cannot be escaped and the sequence cannot
 * cross a line; unterminated is diagnosed and the token ends at line end.
 */
qcc_status qcc_lx_scan_header_name(const qcc_source *src,
                                   qcc_diag_sink *diags, size_t start,
                                   char open, qcc_lx_scan *out);

#endif /* QCC_LEXER_INTERNAL_SCAN_H */
