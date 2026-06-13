/*
 * qcc — preprocessor internals: #if controlling-expression evaluation (§6.10.1)
 *
 * Responsibility
 * Evaluate the controlling constant expression of a #if or #elif directive to a
 * boolean. The pipeline is exactly the standard's (§6.10.1 ¶1, ¶4):
 *
 *   1. Replace each `defined X` / `defined ( X )` by 1 or 0 (whether X is a
 *      currently-defined macro). This happens *before* macro expansion so that
 *      `defined` sees macro names, not their replacements.
 *   2. Macro-expand the remaining tokens.
 *   3. Replace every identifier still present (including ones spelled like
 *      keywords) with the pp-number 0.
 *   4. Parse and evaluate the result as an integer constant expression in
 *      intmax_t / uintmax_t with C's operator precedence and the usual
 *      arithmetic conversions; a nonzero result means the group is included.
 *
 * Errors (syntax, a floating constant, division by zero, leftover tokens) are
 * reported as diagnostics and the expression evaluates to false, so processing
 * continues.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_CEVAL_H
#define QCC_PP_INTERNAL_CEVAL_H

#include <stddef.h>

#include "pp/pp.h"
#include "status/status.h"

/*
 * Evaluate the controlling expression formed by `toks` (the tokens of the
 * directive line after `#if`/`#elif`, up to the newline). Writes 1/0 to
 * *out_true. Returns QCC_OK (even when a diagnostic was emitted and the result
 * defaulted to false) or a hard fault (QCC_ERR_OUT_OF_MEMORY).
 */
qcc_status qcc_pp_eval_condition(qcc_pp *pp, const qcc_ptok *toks, size_t count,
                                 int *out_true);

#endif /* QCC_PP_INTERNAL_CEVAL_H */
