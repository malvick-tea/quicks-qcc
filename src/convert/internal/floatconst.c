/*
 * qcc — convert internals: floating-constant evaluation (implementation).
 *
 * See floatconst.h. The lexeme is already known to be a floating constant
 * (§6.4.4.2). We read the optional f/l suffix to fix the type, then hand the
 * numeric part to the host strtod (a seed-CRT dependency, ADR-0009), which is
 * correctly rounded and parses both decimal and hex floats. strtod stops at the
 * suffix character, so the consumed length must equal the numeric part exactly;
 * anything else is a malformed constant.
 */
#include "convert/internal/floatconst.h"

#include <errno.h>
#include <stdlib.h>

qcc_status qcc_eval_floating(const char *s, size_t n, const qcc_source *src,
                             size_t offset, qcc_diag_sink *diags,
                             double *out_value, qcc_float_type *out_type)
{
    *out_value = 0.0;
    *out_type  = QCC_FLOAT_DOUBLE;

    /* §6.4.4.2 ¶4: the suffix fixes the type and is not part of the number. */
    qcc_float_type type = QCC_FLOAT_DOUBLE;
    size_t         core = n;
    if (n > 0) {
        char last = s[n - 1];
        if (last == 'f' || last == 'F') {
            type = QCC_FLOAT_FLOAT;
            core = n - 1;
        } else if (last == 'l' || last == 'L') {
            type = QCC_FLOAT_LDOUBLE;
            core = n - 1;
        }
    }
    *out_type = type;

    /* The spelling is interned and NUL-terminated, so strtod is safe to call on
       it; it parses the numeric prefix and leaves `end` at the suffix. */
    errno = 0;
    char  *end = NULL;
    double v   = strtod(s, &end);

    if ((size_t)(end - s) != core) {
        return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, n,
                             "'%.*s' is not a valid floating constant",
                             (int)n, s);
    }
    if (errno == ERANGE) {
        /* Overflow yields ±HUGE_VAL, underflow a (sub)normal near 0; both are
           values, but an overflow is worth a warning. */
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, n,
                                      "floating constant out of range");
        if (st != QCC_OK) {
            return st;
        }
    }

    *out_value = v;
    return QCC_OK;
}
