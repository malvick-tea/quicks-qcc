/*
 * qcc — the preprocessor: translation phase 4 (ISO C11 §6.10)
 *
 * Responsibility
 * Turn the preprocessing-token stream of one translation unit into the
 * preprocessing-token stream that phase 7 (the `convert` stage) consumes:
 * execute preprocessing directives (§6.10) — `#include`, `#define`/`#undef`,
 * conditional inclusion, `#line`/`#error`/`#pragma` — and macro-expand the
 * remaining text. This is translation phase 4 of §5.1.1.2.
 *
 * Why a second token type (qcc_ptok) distinct from the lexer's qcc_pp_token?
 *   The lexer's token is *span-backed*: an (offset, length) into one qcc_source,
 *   with the spelling recovered on demand. That is optimal for a single file
 *   with no rewriting and wrong for the preprocessor, which (1) spans many
 *   sources via #include, (2) synthesizes tokens whose spelling exists in no
 *   source — `##` pastes two tokens (§6.10.3.3), `#` builds a string literal
 *   (§6.10.3.2) — and (3) must carry per-token expansion state (the hide set,
 *   §6.10.3.4). So pp operates on a *materialized* token that always owns its
 *   spelling (interned) and records its provenance and hide set. The single
 *   materialization step lives at the lexer boundary; everything above sees
 *   qcc_ptok only. (Design recorded in ADR-0014.)
 *
 * Ownership
 *   A qcc_pp owns an arena and an interner; every qcc_ptok spelling points into
 *   them. Tokens produced by qcc_pp_run are therefore valid only until the
 *   qcc_pp is disposed — keep the preprocessor alive as long as you use its
 *   output. The output qcc_ptok_list owns its (heap) item array and is disposed
 *   separately.
 *
 * Standard: ISO/IEC 9899 (C11) §6.10 and §5.1.1.2. Builds on `lexer` (phases
 * 1-3), `arena`, `intern`, `diag`, `source`, `token`, `status`.
 */
#ifndef QCC_PP_PP_H
#define QCC_PP_PP_H

#include <stddef.h>
#include <stdint.h>

#include "arena/arena.h"
#include "diag/diag.h"
#include "intern/intern.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/* The hide set, macro table, conditional stack, and include resolver are
   preprocessor internals (pp/internal/{hideset,macro,cond,incl}.h); callers of
   this header only hold opaque pointers to them. */
typedef struct qcc_hideset qcc_hideset;
struct qcc_macro_table;
struct qcc_cond_stack;
struct qcc_incl;

/*
 * A materialized preprocessing token. A value: copy it freely. Unlike the
 * lexer's qcc_pp_token, it owns its spelling (interned, so equal spellings share
 * a pointer) and records where it came from for diagnostics and the __FILE__/
 * __LINE__ macros.
 *
 *   kind/punct      : as in token.h (punct valid iff kind == QCC_PP_TOKEN_PUNCT).
 *   spelling        : interned, NUL-terminated, never NULL (length excludes NUL).
 *   source/offset   : provenance. For a token read from input, the source it was
 *                     lexed from and the byte offset within it. For a synthesized
 *                     token (from # or ##), source is the macro's source and
 *                     offset points at the construct that produced it, so
 *                     diagnostics still land somewhere meaningful.
 *   line/column     : 1-based PHYSICAL location of `offset` in `source` (cached).
 *   presumed_line   : the §6.10.4 presumed line — equal to `line` unless a
 *                     `#line` directive shifted it; this is what `__LINE__`
 *                     reports (§6.10.8.1).
 *   presumed_file   : the §6.10.4 presumed file name (interned), or NULL to use
 *                     `source->name`; this is what `__FILE__` reports.
 *   leading_space   : whitespace/comment preceded this token (§6.10.3.2 spacing).
 *   at_line_start   : first token on its logical line (a directive's '#' test).
 *   hideset         : macro names not to expand into this token (§6.10.3.4);
 *                     the empty set is NULL.
 */
typedef struct qcc_ptok {
    qcc_pp_token_kind  kind;
    qcc_punct          punct;
    const char        *spelling;
    size_t             spelling_len;
    const qcc_source  *source;
    size_t             offset;
    uint32_t           line;
    uint32_t           column;
    uint32_t           presumed_line;
    const char        *presumed_file;
    unsigned           leading_space : 1;
    unsigned           at_line_start : 1;
    const qcc_hideset *hideset;
} qcc_ptok;

/*
 * A growable array of qcc_ptok. The item array is heap-owned (seed allocator);
 * the tokens' spellings are NOT owned here (they live in the producing qcc_pp's
 * interner). Use the functions below; treat the fields as private.
 */
typedef struct qcc_ptok_list {
    qcc_ptok *items;
    size_t    count;
    size_t    capacity;
} qcc_ptok_list;

/* Initialize an empty list. Always succeeds. */
void qcc_ptok_list_init(qcc_ptok_list *list);

/* Free the item array and zero the list (does not touch token spellings). */
void qcc_ptok_list_dispose(qcc_ptok_list *list);

/* Append a copy of *tok, growing geometrically. Returns QCC_OK or
   QCC_ERR_OUT_OF_MEMORY / QCC_ERR_INVALID_ARGUMENT. */
qcc_status qcc_ptok_list_push(qcc_ptok_list *list, const qcc_ptok *tok);

/* Drop all items but keep the allocated capacity for reuse. */
void qcc_ptok_list_clear(qcc_ptok_list *list);

/*
 * The preprocessor. Owns the arena and interner that back its tokens, macro
 * definitions, and hide sets. Treat the fields as private; use the functions
 * below. Diagnostics are reported to the borrowed sink.
 */
typedef struct qcc_pp {
    qcc_arena      arena;     /* Backs synthesized spellings, macros, hide sets. */
    qcc_intern     interner;  /* Interns every token spelling (over `arena`).    */
    qcc_diag_sink *diags;     /* Borrowed; must outlive the preprocessor.        */
    struct qcc_macro_table *macros; /* Macro definitions in force (arena-owned). */
    struct qcc_cond_stack  *conds;  /* Open #if/#ifdef conditionals (arena-owned)*/
    struct qcc_incl        *includes; /* #include search paths + owned sources.   */
} qcc_pp;

/*
 * Initialize a preprocessor that reports to `diags` (must be non-NULL and
 * outlive the preprocessor). Returns QCC_OK or QCC_ERR_INVALID_ARGUMENT /
 * QCC_ERR_OUT_OF_MEMORY; on failure *pp is safe to pass to qcc_pp_dispose.
 */
qcc_status qcc_pp_init(qcc_pp *pp, qcc_diag_sink *diags);

/* Release everything the preprocessor owns (arena, interner, included sources).
   After this, any qcc_ptok it produced is invalid. Idempotent and NULL-safe. */
void qcc_pp_dispose(qcc_pp *pp);

/*
 * Add a directory to the #include search path. `dir` is copied; "" means the
 * current directory. Call before qcc_pp_run. Returns QCC_OK or
 * QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY.
 *
 *   qcc_pp_add_include_dir       : an angle/system dir (a -I path) — searched by
 *                                  `#include <...>` and by the `"..."` fallback.
 *   qcc_pp_add_quote_include_dir : a quote-only dir (a -iquote path) — searched
 *                                  by `#include "..."`, before the angle dirs.
 *
 * The directory of the including file is always searched first for the `"..."`
 * form, with no registration needed (§6.10.2 ¶3).
 */
qcc_status qcc_pp_add_include_dir(qcc_pp *pp, const char *dir);
qcc_status qcc_pp_add_quote_include_dir(qcc_pp *pp, const char *dir);

/*
 * Intern a byte span through the preprocessor's interner and return the stable
 * pointer (NULL on OOM). Exposed so callers/tests can obtain the canonical
 * pointer for a spelling to compare against tokens (pointer identity).
 */
const char *qcc_pp_intern(qcc_pp *pp, const char *bytes, size_t length);

/*
 * Preprocess one source completely, appending the resulting tokens — terminated
 * by a QCC_PP_TOKEN_EOF token — to `out`. `out` must be an initialized list
 * (its prior contents are kept; tokens are appended).
 *
 * Returns QCC_OK on success (including when diagnostics were emitted for
 * recoverable errors — check the sink's error count), QCC_ERR_PP if a
 * preprocessing error makes the result unusable, or QCC_ERR_* for hard faults.
 */
qcc_status qcc_pp_run(qcc_pp *pp, const qcc_source *source, qcc_ptok_list *out);

#endif /* QCC_PP_PP_H */
