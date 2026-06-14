/*
 * qcc — convert internals: integer-constant evaluation (ISO C11 §6.4.4.1)
 *
 * Responsibility
 * Compute the value and type of an integer-constant lexeme: parse the base
 * (decimal, octal `0…`, or hexadecimal `0x…`), accumulate the digits with
 * overflow detection, read the optional `u`/`l`/`ll` suffix, and choose the
 * type as the first of the constant's candidate list (§6.4.4.1 ¶5) whose
 * representation holds the value, against the target's widths (x86-64 System V
 * LP64: int 32-bit, long and long long 64-bit).
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_INTCONST_H
#define QCC_CONVERT_INTERNAL_INTCONST_H

#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Evaluate the integer-constant lexeme s[0..n) — which `convert` has already
 * classified as an integer constant, so it has no '.' or exponent. Writes the
 * value to *out_value and the type to *out_type. A malformed constant (a bad
 * digit for its base, an invalid suffix, or a value too large for any type) is
 * reported to `diags` at `src`/`offset`; a best-effort value/type is still
 * produced so conversion continues. Returns QCC_OK or a hard fault
 * (QCC_ERR_OUT_OF_MEMORY while recording a diagnostic).
 */
qcc_status qcc_eval_integer(const char *s, size_t n, const qcc_source *src,
                            size_t offset, qcc_diag_sink *diags,
                            uint64_t *out_value, qcc_int_type *out_type);

#endif /* QCC_CONVERT_INTERNAL_INTCONST_H */
