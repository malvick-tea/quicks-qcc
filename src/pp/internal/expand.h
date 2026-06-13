/*
 * qcc — preprocessor internals: macro expansion (ISO C11 §6.10.3, §6.10.3.4)
 *
 * Responsibility
 * Given a macro-name token that the driver found is currently defined and not
 * blocked by its own hide set, perform one expansion step: compute the new hide
 * set, substitute the replacement list, and push the result onto the token
 * stream so it is rescanned for further macros. Rescanning falls out of pushing
 * onto the stream — the driver just keeps calling next() (ADR-0014).
 *
 * Hide sets (§6.10.3.4) are the recursion guard: an object-like expansion adds
 * the macro's own name to every replacement token's hide set, so the macro
 * cannot expand within its own expansion. Function-like expansion (added next)
 * additionally intersects the name token's and the closing ')''s hide sets.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_EXPAND_H
#define QCC_PP_INTERNAL_EXPAND_H

#include "pp/internal/macro.h"
#include "pp/internal/stream.h"
#include "pp/pp.h"
#include "status/status.h"

/*
 * Try to expand a use of `macro` named by token `name` (read from `stream`).
 * On success the substituted replacement is pushed onto `stream` and
 * *out_expanded is set to 1; the caller discards `name` and resumes reading.
 * If the use does not expand here (e.g. a function-like macro name not followed
 * by '('), *out_expanded is set to 0 and the caller outputs `name` unchanged.
 *
 * Returns QCC_OK or a hard fault (QCC_ERR_OUT_OF_MEMORY). `name`, `macro`,
 * `pp`, and `stream` must be non-NULL.
 */
qcc_status qcc_pp_expand(qcc_pp *pp, qcc_pp_stream *stream, const qcc_ptok *name,
                         const qcc_macro *macro, int *out_expanded);

#endif /* QCC_PP_INTERNAL_EXPAND_H */
