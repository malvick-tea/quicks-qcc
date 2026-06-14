/*
 * qcc — convert internals: floating-constant evaluation (ISO C11 §6.4.4.2)
 *
 * Responsibility
 * Compute the value and type of a floating-constant lexeme — decimal
 * (`1.5`, `1e10`, `.5`) and hexadecimal (`0x1p4`) forms — and its type from the
 * suffix (`f`/`F` → float, `l`/`L` → long double, none → double, §6.4.4.2 ¶4).
 *
 * Seed dependency (ADR-0009): the value is obtained with the host C library's
 * strtod, which is correctly rounded and already parses both decimal and C99 hex
 * floats. This is a temporary seed-CRT use; when qlibc lands (or self-hosting
 * needs it) it is replaced by our own correctly-rounded string→double, behind
 * this same interface. The value is stored as a double; on this host long double
 * coincides with double, so the long-double suffix records the type without
 * widening the stored value (a documented limitation until a wider float path
 * is needed).
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_FLOATCONST_H
#define QCC_CONVERT_INTERNAL_FLOATCONST_H

#include <stddef.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Evaluate the floating-constant lexeme s[0..n) — which `convert` has classified
 * as a floating constant (it has a '.', a decimal exponent, or a hex 'p'
 * exponent). Writes the value to *out_value and the type to *out_type. A
 * malformed constant, or one whose magnitude overflows, is reported to `diags`
 * at `src`/`offset`; a best-effort value/type is still produced. Returns QCC_OK
 * or a hard fault (QCC_ERR_OUT_OF_MEMORY while recording a diagnostic).
 */
qcc_status qcc_eval_floating(const char *s, size_t n, const qcc_source *src,
                             size_t offset, qcc_diag_sink *diags,
                             double *out_value, qcc_float_type *out_type);

#endif /* QCC_CONVERT_INTERNAL_FLOATCONST_H */
