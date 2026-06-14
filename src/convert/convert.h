/*
 * qcc — convert: preprocessing tokens to tokens (ISO C11 §5.1.1.2 phases 5-7)
 *
 * Responsibility
 * Turn the preprocessing-token stream that `pp` produces (phase 4) into the
 * token stream the parser consumes (§6.4 ¶3). This is the entry to translation
 * phases 5-7: reclassify each preprocessing token as a token — an identifier
 * that spells a keyword becomes a keyword (§6.4.1 ¶2), a pp-number becomes an
 * integer or floating constant (§6.4.4) — evaluate constant values (§6.4.4), and
 * concatenate adjacent string literals (§5.1.1.2 phase 6).
 *
 * Staged delivery (ADR-0017). This module lands in units:
 *   A. reclassification — keyword resolution, pp-number shape classification,
 *      and the trivial category shifts, with stray-token diagnostics. (here)
 *   B. integer/floating constant value + type (§6.4.4.1/.2).
 *   C. character/string value with escape decoding (§6.4.4.4/§6.4.5) and the
 *      phase-6 concatenation of adjacent string literals.
 * Until B/C land, a constant token is identified by `kind` and carries its source
 * lexeme in `spelling` (token.h); no value fields exist yet.
 *
 * Ownership
 *   A qcc_convert owns an arena and an interner; every output token's `spelling`
 *   is interned through them, so the token stream is independent of the
 *   preprocessor's lifetime for its spellings. A token's `source` pointer,
 *   however, borrows the original qcc_source (for diagnostics and, later, value
 *   recovery): those sources — the translation unit and any #include'd files —
 *   must outlive the tokens.
 *
 * Standard: ISO/IEC 9899 (C11) §5.1.1.2, §6.4. Builds on `pp`, `token`, `arena`,
 * `intern`, `diag`, `status`.
 */
#ifndef QCC_CONVERT_CONVERT_H
#define QCC_CONVERT_CONVERT_H

#include <stddef.h>

#include "arena/arena.h"
#include "diag/diag.h"
#include "intern/intern.h"
#include "pp/pp.h"
#include "status/status.h"
#include "token/token.h"

/*
 * A growable array of qcc_token. The item array is heap-owned (seed allocator);
 * the tokens' spellings are NOT owned here (they live in the producing
 * qcc_convert's interner). Use the functions; treat the fields as private.
 */
typedef struct qcc_token_list {
    qcc_token *items;
    size_t     count;
    size_t     capacity;
} qcc_token_list;

void qcc_token_list_init(qcc_token_list *list);
void qcc_token_list_dispose(qcc_token_list *list);
qcc_status qcc_token_list_push(qcc_token_list *list, const qcc_token *tok);
void qcc_token_list_clear(qcc_token_list *list);

/*
 * The converter. Owns the arena and interner backing its tokens' spellings.
 * Treat the fields as private; use the functions. Diagnostics go to the borrowed
 * sink, which must outlive the converter.
 */
typedef struct qcc_convert {
    qcc_arena      arena;
    qcc_intern     interner;
    qcc_diag_sink *diags;   /* Borrowed; must outlive the converter. */
} qcc_convert;

/*
 * Initialize a converter reporting to `diags` (non-NULL, must outlive it).
 * Returns QCC_OK or QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY; on failure
 * *cv is safe to pass to qcc_convert_dispose.
 */
qcc_status qcc_convert_init(qcc_convert *cv, qcc_diag_sink *diags);

/* Release the arena and interner. After this, any token it produced is invalid.
   Idempotent and NULL-safe. */
void qcc_convert_dispose(qcc_convert *cv);

/*
 * Convert the preprocessing-token stream `in` (a qcc_pp_run result, terminated
 * by a QCC_PP_TOKEN_EOF) into tokens, appending them — terminated by a
 * QCC_TOKEN_EOF — to `out` (an initialized list; prior contents are kept).
 *
 * Returns QCC_OK on success (including when recoverable diagnostics were emitted
 * — check the sink's error count) or QCC_ERR_* for hard faults.
 */
qcc_status qcc_convert_run(qcc_convert *cv, const qcc_ptok_list *in,
                           qcc_token_list *out);

#endif /* QCC_CONVERT_CONVERT_H */
