/*
 * qcc — integer constant-expression evaluator (ISO C11 §6.6; design ADR-0025)
 *
 * Responsibility
 * Decide whether a parsed expression (`qcc_expr`, §6.5) is an *integer constant
 * expression* (§6.6) and, if so, compute its value with conforming C semantics —
 * the integer promotions (§6.3.1.1) and the usual arithmetic conversions
 * (§6.3.1.8) on the x86-64 System V LP64 target (so signedness and width drive the
 * results of `/`, `%`, `>>`, the comparisons, and overflow wrap-around). This is
 * what the array-bound parser (§6.7.6.2) needs now, and what `enum` enumerators
 * (§6.7.2.2), `case` labels (§6.8.4.2), bit-field widths (§6.7.2.1), and
 * `_Static_assert` (§6.7.10) will use as they land.
 *
 * Not the preprocessor's `#if` evaluator
 *   `pp/internal/ceval` evaluates `#if` lines under the special §6.10.1 rules
 *   (every operand is `intmax_t`/`uintmax_t`, undefined identifiers are 0). This
 *   module instead evaluates a phase-7 `qcc_expr` over real C types, which is a
 *   different problem (ADR-0025).
 *
 * Purity
 *   No allocation, no diagnostics, no symbol table: it returns a status and a
 *   value. The caller decides whether "not a constant" is an error (an array bound
 *   silently treats it as an unknown bound; `enum`/`case`/`_Static_assert` report
 *   it), and emits the diagnostic with the right context.
 *
 * Standard: ISO/IEC 9899 (C11) §6.6, §6.3.1.1, §6.3.1.8. Builds on `ast`, `type`,
 * `token`, `status`.
 */
#ifndef QCC_CONSTEVAL_CONSTEVAL_H
#define QCC_CONSTEVAL_CONSTEVAL_H

#include <stdint.h>

#include "ast/ast.h"
#include "status/status.h"
#include "token/token.h"

/*
 * The value of an evaluated integer constant expression. `value` is the result's
 * two's-complement bit pattern reduced to `type`'s width: sign-extended to 64 bits
 * for a signed `type`, zero-extended for an unsigned one. So `(int64_t)value` is
 * the signed interpretation and `value` (masked to the type's width) the unsigned
 * one. `type` is the LP64 integer type of the result (a sub-`int` result — e.g.
 * from a cast to `char` — is reported promoted to `int`, §6.3.1.1).
 */
typedef struct qcc_const_value {
    uint64_t     value;
    qcc_int_type type;
} qcc_const_value;

/*
 * Evaluate `e` as an integer constant expression (§6.6). On success returns QCC_OK
 * and fills *out. Returns QCC_ERR_TYPE when `e` is not an integer constant
 * expression this module can evaluate — a non-constant operand (an identifier, a
 * call, an assignment, `sizeof` of an *expression*), a floating operand, the comma
 * operator, or division/remainder by zero. Returns QCC_ERR_INVALID_ARGUMENT if `e`
 * or `out` is NULL. `&&`, `||`, and `?:` short-circuit, so an unevaluated operand
 * need not itself be constant (§6.6 ¶3) — `1 || (1/0)` evaluates to 1.
 */
qcc_status qcc_eval_const_int(const qcc_expr *e, qcc_const_value *out);

#endif /* QCC_CONSTEVAL_CONSTEVAL_H */
