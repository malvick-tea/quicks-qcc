/*
 * qcc — render a preprocessing-token stream back to text (the `-E` output)
 *
 * Responsibility
 * Turn the phase-4 qcc_ptok stream that qcc_pp_run produces into human-readable
 * preprocessed C text — what `cpp -E` / `qcc -E` prints. Phase-4 output carries
 * no newline tokens (line structure was consumed, pp.h), so the renderer
 * reconstructs lines from each token's at_line_start flag: a file token that
 * began a logical line starts a new output line; macro-expansion output, which
 * never begins a line, stays inline. Within a line tokens are separated by the
 * original inter-token spacing (leading_space), with a space inserted wherever
 * omitting one would let two adjacent tokens re-lex as a different token (e.g.
 * `+ +` must not become `++`). The result therefore re-lexes to the same token
 * sequence — the property `-E` output must have.
 *
 * Not reproduced (a documented v1 limitation): blank lines and exact original
 * line numbers. A later `-E` can restore precise numbering by emitting `# line`
 * markers from each token's presumed_line (§6.10.4); the token model already
 * carries it.
 *
 * This is a pp-level utility (it speaks qcc_ptok), kept I/O-free: it builds a
 * heap string and the CLI writes it. Standard: ISO/IEC 9899 (C11) §5.1.1.2.
 */
#ifndef QCC_PP_RENDER_H
#define QCC_PP_RENDER_H

#include <stddef.h>

#include "pp/pp.h"
#include "status/status.h"

/*
 * Render `toks` (a qcc_pp_run result, terminated by a QCC_PP_TOKEN_EOF that is
 * not emitted) into a freshly malloc'd, NUL-terminated C string. On success
 * *out_text owns the buffer (free it with free()) and *out_len is its length
 * excluding the NUL. The output ends with a newline when it is non-empty.
 *
 * Returns QCC_OK, or QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY. On
 * failure *out_text is set to NULL and *out_len to 0.
 */
qcc_status qcc_pp_render(const qcc_ptok_list *toks, char **out_text,
                         size_t *out_len);

#endif /* QCC_PP_RENDER_H */
