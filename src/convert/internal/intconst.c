/*
 * qcc — convert internals: integer-constant evaluation (implementation).
 *
 * See intconst.h. The lexeme is already known to be an integer constant
 * (§6.4.4.1) — `convert` classified it by shape — so it is base-prefix, digits,
 * and an optional suffix, with no '.' or exponent. We parse it by hand (no
 * strtoull) so the same code can run under the self-hosted toolchain later.
 */
#include "convert/internal/intconst.h"

/* Target widths, as the largest representable value (x86-64 System V LP64). */
#define QCC_INT_MAX_U   ((uint64_t)0x7FFFFFFFu)         /* int                */
#define QCC_UINT_MAX_U  ((uint64_t)0xFFFFFFFFu)         /* unsigned int       */
#define QCC_LONG_MAX_U  ((uint64_t)0x7FFFFFFFFFFFFFFFu) /* long == long long  */
/* unsigned long / unsigned long long hold every uint64_t, so they always fit. */

/* Digit value of `c` in `base`, or -1 if it is not a digit of that base. */
static int digit_value(char c, int base)
{
    int v;
    if (c >= '0' && c <= '9') {
        v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
    } else {
        return -1;
    }
    return (v < base) ? v : -1;
}

/* Does `value` fit the representation of type `t` (LP64 widths)? */
static int fits(uint64_t value, qcc_int_type t)
{
    switch (t) {
    case QCC_INT_INT:    return value <= QCC_INT_MAX_U;
    case QCC_INT_UINT:   return value <= QCC_UINT_MAX_U;
    case QCC_INT_LONG:   return value <= QCC_LONG_MAX_U;
    case QCC_INT_ULONG:  return 1;
    case QCC_INT_LLONG:  return value <= QCC_LONG_MAX_U;
    case QCC_INT_ULLONG: return 1;
    }
    return 0;
}

qcc_status qcc_eval_integer(const char *s, size_t n, const qcc_source *src,
                            size_t offset, qcc_diag_sink *diags,
                            uint64_t *out_value, qcc_int_type *out_type)
{
    *out_value = 0;
    *out_type  = QCC_INT_INT;

    int    base       = 10;
    int    is_decimal = 1;
    size_t i          = 0;
    if (n >= 1 && s[0] == '0') {
        if (n >= 2 && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            i    = 2;
        } else {
            base = 8; /* A leading 0 is an octal constant; "0" is octal zero. */
            i    = 1;
        }
        is_decimal = 0;
    }

    /* Accumulate digits until a non-digit (the start of the suffix). */
    size_t   digits_start = i;
    uint64_t value        = 0;
    int      overflow     = 0;
    while (i < n) {
        int d = digit_value(s[i], base);
        if (d < 0) {
            break;
        }
        if (value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
            overflow = 1;
        }
        value = value * (uint64_t)base + (uint64_t)d;
        ++i;
    }

    if (base == 16 && i == digits_start) {
        return qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, n,
                             "'%.*s' is not a valid hexadecimal constant",
                             (int)n, s);
    }

    /* Suffix: u/U once and one of l/L/ll/LL, in either order (§6.4.4.1 ¶1). */
    int u_seen   = 0;
    int l_rank   = 0; /* 0 none, 1 long, 2 long long. */
    int bad      = 0;
    while (i < n && !bad) {
        char c = s[i];
        if (c == 'u' || c == 'U') {
            if (u_seen) {
                bad = 1;
            } else {
                u_seen = 1;
                ++i;
            }
        } else if (c == 'l' || c == 'L') {
            if (l_rank != 0) {
                bad = 1;
            } else if (i + 1 < n && s[i + 1] == c) {
                l_rank = 2; /* "ll"/"LL"; a mixed "lL" is rejected below. */
                i += 2;
            } else {
                l_rank = 1;
                ++i;
            }
        } else {
            bad = 1;
        }
    }

    if (bad || i != n) {
        if (base == 8 && i < n && (s[i] == '8' || s[i] == '9')) {
            qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, n,
                                          "invalid digit '%c' in octal constant",
                                          s[i]);
            if (st != QCC_OK) {
                return st;
            }
        } else {
            qcc_status st = qcc_diag_emit(diags, QCC_DIAG_ERROR, src, offset, n,
                                          "invalid suffix on integer constant "
                                          "'%.*s'", (int)n, s);
            if (st != QCC_OK) {
                return st;
            }
        }
        /* Best-effort: keep the value and whatever suffix bits were valid. */
    }

    if (overflow) {
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, n,
                                      "integer constant is too large for any "
                                      "integer type");
        if (st != QCC_OK) {
            return st;
        }
    }

    /* Candidate type lists (§6.4.4.1 ¶5). Decimal constants without a 'u' suffix
       consider only signed types; octal/hex also consider the unsigned ones. */
    static const qcc_int_type dec_none[] = { QCC_INT_INT, QCC_INT_LONG,
                                             QCC_INT_LLONG };
    static const qcc_int_type oh_none[]  = { QCC_INT_INT, QCC_INT_UINT,
                                             QCC_INT_LONG, QCC_INT_ULONG,
                                             QCC_INT_LLONG, QCC_INT_ULLONG };
    static const qcc_int_type u_only[]   = { QCC_INT_UINT, QCC_INT_ULONG,
                                             QCC_INT_ULLONG };
    static const qcc_int_type dec_l[]    = { QCC_INT_LONG, QCC_INT_LLONG };
    static const qcc_int_type oh_l[]     = { QCC_INT_LONG, QCC_INT_ULONG,
                                             QCC_INT_LLONG, QCC_INT_ULLONG };
    static const qcc_int_type ul[]       = { QCC_INT_ULONG, QCC_INT_ULLONG };
    static const qcc_int_type dec_ll[]   = { QCC_INT_LLONG };
    static const qcc_int_type oh_ll[]    = { QCC_INT_LLONG, QCC_INT_ULLONG };
    static const qcc_int_type ull[]      = { QCC_INT_ULLONG };

    const qcc_int_type *cands;
    size_t              ncands;
    if (u_seen && l_rank == 0) {
        cands = u_only;  ncands = 3;
    } else if (u_seen && l_rank == 1) {
        cands = ul;      ncands = 2;
    } else if (u_seen) { /* l_rank == 2 */
        cands = ull;     ncands = 1;
    } else if (l_rank == 0) {
        if (is_decimal) { cands = dec_none; ncands = 3; }
        else            { cands = oh_none;  ncands = 6; }
    } else if (l_rank == 1) {
        if (is_decimal) { cands = dec_l; ncands = 2; }
        else            { cands = oh_l;  ncands = 4; }
    } else { /* l_rank == 2, no u */
        if (is_decimal) { cands = dec_ll; ncands = 1; }
        else            { cands = oh_ll;  ncands = 2; }
    }

    qcc_int_type chosen   = cands[ncands - 1];
    int          fitted   = 0;
    for (size_t k = 0; k < ncands; ++k) {
        if (fits(value, cands[k])) {
            chosen = cands[k];
            fitted = 1;
            break;
        }
    }
    if (!fitted) {
        /* A decimal constant with no unsigned candidate that exceeds long long:
           §6.4.4.1 ¶6 leaves it typeless; like GCC we make it unsigned long long
           with a warning rather than reject the program. */
        qcc_status st = qcc_diag_emit(diags, QCC_DIAG_WARNING, src, offset, n,
                                      "integer constant is so large that it is "
                                      "unsigned");
        if (st != QCC_OK) {
            return st;
        }
        chosen = QCC_INT_ULLONG;
    }

    *out_value = value;
    *out_type  = chosen;
    return QCC_OK;
}
