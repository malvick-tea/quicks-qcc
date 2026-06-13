/*
 * qcc — preprocessor internals: the # and ## operators (ISO C11 §6.10.3.2/.3)
 *
 * Responsibility
 * The two operators that *synthesize* preprocessing tokens during function-like
 * macro substitution:
 *
 *   #  (stringize, §6.10.3.2): turn a macro argument's preprocessing tokens into
 *      a single string-literal token — original spacing collapsed to one space
 *      between tokens, and a '\' inserted before each '"' and '\' that appears in
 *      a string-literal or character-constant token.
 *   ## (paste, §6.10.3.3): concatenate the spellings of two tokens and re-lex the
 *      result; it must form exactly one preprocessing token, else the program is
 *      ill-formed (the standard says undefined — we diagnose).
 *
 * Both produce a materialized qcc_ptok whose spelling is interned through the
 * preprocessor and whose provenance points at the construct that produced it, so
 * later diagnostics land somewhere real.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_GLUE_H
#define QCC_PP_INTERNAL_GLUE_H

#include <stddef.h>

#include "pp/pp.h"
#include "status/status.h"

/*
 * Stringize `count` argument tokens (the *unexpanded* argument) into a single
 * string-literal token written to *out (§6.10.3.2). `anchor` supplies the
 * result's provenance (typically the macro-name token). Returns QCC_OK or
 * QCC_ERR_OUT_OF_MEMORY. An empty argument yields the token `""`.
 */
qcc_status qcc_pp_stringize(qcc_pp *pp, const qcc_ptok *toks, size_t count,
                            const qcc_ptok *anchor, qcc_ptok *out);

/*
 * Paste `left` and `right` (§6.10.3.3): concatenate their spellings, re-lex, and
 * if the concatenation is exactly one preprocessing token, write it to *out and
 * set *out_ok = 1. If it is not a single valid token, emit a diagnostic at
 * `anchor` and set *out_ok = 0 (with *out left as `left`, the conventional
 * recovery). Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY. `anchor` supplies the
 * result's provenance.
 */
qcc_status qcc_pp_paste(qcc_pp *pp, const qcc_ptok *left, const qcc_ptok *right,
                        const qcc_ptok *anchor, qcc_ptok *out, int *out_ok);

#endif /* QCC_PP_INTERNAL_GLUE_H */
