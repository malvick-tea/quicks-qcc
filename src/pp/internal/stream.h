/*
 * qcc — preprocessor internals: the token-input stream (ISO C11 §6.10, §6.4.7)
 *
 * Responsibility
 * Be the single place the preprocessor pulls preprocessing tokens from, hiding
 * two facts behind one next()/push interface:
 *
 *   1. Tokens come from a *stack* of inputs. The bottom is the translation
 *      unit's file (a lexer over a qcc_source); macro expansion pushes the
 *      substituted replacement list on top to be rescanned (§6.10.3.4), and
 *      #include (added later) pushes another file. next() reads from the top
 *      input and transparently pops exhausted ones, so the rescan loop and
 *      nested includes are just "keep calling next()".
 *   2. A lexer input yields the lexer's span-backed qcc_pp_token, which is
 *      *materialized* here into a qcc_ptok (spelling interned, provenance and
 *      an empty hide set recorded). This is the one boundary between the two
 *      token vocabularies (ADR-0014); above the stream everything is qcc_ptok.
 *
 * next() also reports whether the token came from a macro expansion, because a
 * '#' that begins a directive must come from a file at the start of a line and
 * NOT from a macro replacement (§6.10.3.4 ¶3: a '#' produced by expansion is not
 * a directive).
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_STREAM_H
#define QCC_PP_INTERNAL_STREAM_H

#include <stddef.h>

#include "lexer/lexer.h"
#include "pp/pp.h"
#include "source/source.h"
#include "status/status.h"

/* Which kind of input a stack frame is. */
typedef enum qcc_pp_input_kind {
    QCC_PP_INPUT_LEXER,   /* A lexer over a qcc_source (file / #include).        */
    QCC_PP_INPUT_TOKENS   /* A fixed qcc_ptok array (macro replacement rescan).  */
} qcc_pp_input_kind;

/*
 * One input on the stack. Heap-allocated (seed allocator) and freed on pop. For
 * a TOKENS input, `toks` is borrowed and must stay valid until the input is
 * popped — expansion allocates it in the preprocessor arena, which outlives the
 * frame.
 */
typedef struct qcc_pp_input {
    qcc_pp_input_kind    kind;
    struct qcc_pp_input *below;   /* Next input down the stack, or NULL.         */

    qcc_lexer            lexer;    /* LEXER: the lexer state.                     */
    const qcc_source    *source;   /* LEXER: the source being lexed.             */

    const qcc_ptok      *toks;     /* TOKENS: borrowed token array.              */
    size_t               count;    /* TOKENS: number of tokens.                  */
    size_t               pos;      /* TOKENS: next index to yield.               */
} qcc_pp_input;

/*
 * The stream. Owns its input stack and a reusable scratch buffer for
 * materialization. Borrows the qcc_pp (for the interner and diagnostics sink).
 * Treat the fields as private; use the functions below.
 */
typedef struct qcc_pp_stream {
    qcc_pp        *pp;          /* Borrowed: interner + diags.                   */
    qcc_pp_input  *top;         /* Current input; NULL once fully exhausted.     */
    char          *scratch;     /* Spelling-recovery buffer (grows, reused).     */
    size_t         scratch_cap;
    qcc_ptok       pending;     /* One-token pushback (qcc_pp_stream_unget).     */
    int            has_pending; /* 1 if `pending` is live.                       */
    int            pending_from_expansion; /* from_expansion to report for it.   */
} qcc_pp_stream;

/*
 * Initialize a stream whose bottom input lexes `source`. Returns QCC_OK or
 * QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY; on failure the stream is
 * safe to pass to qcc_pp_stream_dispose.
 */
qcc_status qcc_pp_stream_init(qcc_pp_stream *stream, qcc_pp *pp,
                              const qcc_source *source);

/*
 * Initialize a stream whose bottom input is a fixed token array (rather than a
 * file lexer). Used to macro-expand a macro argument in isolation — "as if it
 * formed the rest of the preprocessing file" (§6.10.3.1 ¶1). `toks` must outlive
 * the stream (allocate it in the arena). Returns QCC_OK or an init error.
 */
qcc_status qcc_pp_stream_init_tokens(qcc_pp_stream *stream, qcc_pp *pp,
                                     const qcc_ptok *toks, size_t count);

/* Free the input stack and scratch buffer; zero the stream. NULL-safe. */
void qcc_pp_stream_dispose(qcc_pp_stream *stream);

/*
 * Push one token back so the next qcc_pp_stream_next returns it (with the given
 * from_expansion). Exactly one token of pushback is supported; ungetting twice
 * without an intervening next returns QCC_ERR_INVALID_ARGUMENT. Used to look one
 * token ahead for the '(' of a function-like macro invocation (§6.10.3 ¶10).
 */
qcc_status qcc_pp_stream_unget(qcc_pp_stream *stream, const qcc_ptok *tok,
                               int from_expansion);

/*
 * Produce the next token into *out. After the last token, *out is a
 * QCC_PP_TOKEN_EOF and every later call repeats EOF (safe to over-read).
 * *from_expansion (must be non-NULL) is set to 1 if the token came from a macro
 * replacement (a TOKENS input), 0 if it came directly from a file lexer — the
 * directive layer uses this so an expansion-produced '#' is never a directive.
 *
 * Returns QCC_OK (including for tokens carrying diagnosed lexical errors), or a
 * hard fault (QCC_ERR_OUT_OF_MEMORY / QCC_ERR_INVALID_ARGUMENT).
 */
qcc_status qcc_pp_stream_next(qcc_pp_stream *stream, qcc_ptok *out,
                              int *from_expansion);

/*
 * Push a token array as a new top input, to be yielded before whatever is
 * currently next (used to rescan a macro's substituted replacement). `toks`
 * must remain valid until the input is exhausted (allocate it in the arena).
 * A zero-length push is a no-op. Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY.
 */
qcc_status qcc_pp_stream_push_tokens(qcc_pp_stream *stream, const qcc_ptok *toks,
                                     size_t count);

#endif /* QCC_PP_INTERNAL_STREAM_H */
