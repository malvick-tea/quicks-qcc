/*
 * qcc — integer constant-expression evaluator (implementation).
 *
 * See consteval.h and ADR-0025. Each subexpression evaluates to an `ival` — a
 * value plus its width and signedness — modeling C's integer types directly so
 * that the integer promotions (§6.3.1.1) and usual arithmetic conversions
 * (§6.3.1.8) produce conforming results on the x86-64 System V LP64 target
 * (int 32-bit, long/long long 64-bit). `bits` always holds the value reduced to
 * (width, is_unsigned): sign-extended to 64 bits when signed, zero-extended when
 * unsigned, so 64-bit arithmetic on `bits` followed by one reduce() at the result
 * width gives the right wrap-around for every operator.
 */
#include "consteval/consteval.h"

#include "type/type.h"

/* An evaluated integer value: `bits` reduced to (width, uns) as described above. */
typedef struct ival {
    uint64_t bits;
    int      width; /* 8, 16, 32, or 64 */
    int      uns;   /* 1 if unsigned */
} ival;

/* Low-`width` mask (all ones for width 64). */
static uint64_t mask_for(int width)
{
    return (width >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1u);
}

/* Reduce a raw 64-bit result to the (width, uns) representation: keep the low
   `width` bits, then sign-extend to 64 bits if signed and the sign bit is set. */
static uint64_t reduce(uint64_t raw, int width, int uns)
{
    uint64_t m   = mask_for(width);
    uint64_t low = raw & m;
    if (!uns && width < 64 && (low & ((uint64_t)1 << (width - 1))) != 0) {
        return low | ~m; /* sign-extend */
    }
    return low;
}

static ival make(uint64_t raw, int width, int uns)
{
    ival v;
    v.width = width;
    v.uns   = uns;
    v.bits  = reduce(raw, width, uns);
    return v;
}

/* The (width, signedness) of an LP64 integer-constant type (§6.4.4.1 ¶5). */
static void int_type_wu(qcc_int_type t, int *width, int *uns)
{
    switch (t) {
    case QCC_INT_INT:    *width = 32; *uns = 0; break;
    case QCC_INT_UINT:   *width = 32; *uns = 1; break;
    case QCC_INT_LONG:   *width = 64; *uns = 0; break;
    case QCC_INT_ULONG:  *width = 64; *uns = 1; break;
    case QCC_INT_LLONG:  *width = 64; *uns = 0; break;
    case QCC_INT_ULLONG: *width = 64; *uns = 1; break;
    default:             *width = 32; *uns = 0; break;
    }
}

/* The LP64 integer type for a (width, signedness) result (long vs long long is a
   value-irrelevant distinction, so a 64-bit result reports as long). */
static qcc_int_type wu_int_type(int width, int uns)
{
    if (width <= 32) {
        return uns ? QCC_INT_UINT : QCC_INT_INT;
    }
    return uns ? QCC_INT_ULONG : QCC_INT_LONG;
}

/* Integer promotion (§6.3.1.1): a type of rank below int becomes int (the value is
   preserved, since int represents every value of a narrower integer type). */
static ival promote(ival v)
{
    if (v.width < 32) {
        return make(v.bits, 32, 0);
    }
    return v;
}

/* The usual arithmetic conversions (§6.3.1.8) of two promoted operands: the common
   (width, signedness) the operation is performed in. */
static void usual_arith(ival a, ival b, int *width, int *uns)
{
    if (a.uns == b.uns) {
        *width = (a.width > b.width) ? a.width : b.width;
        *uns   = a.uns;
        return;
    }
    /* Differing signedness: U the unsigned operand, S the signed one. */
    ival u = a.uns ? a : b;
    ival s = a.uns ? b : a;
    if (u.width >= s.width) {
        *width = u.width; /* the signed one converts to the unsigned type */
        *uns   = 1;
    } else {
        *width = s.width; /* signed has greater rank and represents all of U */
        *uns   = 0;
    }
}

static qcc_status eval(const qcc_expr *e, ival *out);

/* A binary operator whose result type is the usual-arithmetic-conversion common
   type (the additive/multiplicative/bitwise/relational/equality operators); shifts
   and the logical operators are handled separately. */
static qcc_status eval_arith_binary(qcc_punct op, ival a, ival b, ival *out)
{
    int w, u;
    usual_arith(a, b, &w, &u);
    uint64_t av = a.bits;
    uint64_t bv = b.bits;

    switch (op) {
    case QCC_PUNCT_PLUS:    *out = make(av + bv, w, u); return QCC_OK;
    case QCC_PUNCT_MINUS:   *out = make(av - bv, w, u); return QCC_OK;
    case QCC_PUNCT_STAR:    *out = make(av * bv, w, u); return QCC_OK;
    case QCC_PUNCT_AMP:     *out = make(av & bv, w, u); return QCC_OK;
    case QCC_PUNCT_PIPE:    *out = make(av | bv, w, u); return QCC_OK;
    case QCC_PUNCT_CARET:   *out = make(av ^ bv, w, u); return QCC_OK;
    case QCC_PUNCT_SLASH:
    case QCC_PUNCT_PERCENT: {
        if (bv == 0) {
            return QCC_ERR_TYPE; /* division by zero is not a valid constant. */
        }
        uint64_t r;
        if (u) {
            uint64_t x = av & mask_for(w);
            uint64_t y = bv & mask_for(w);
            r = (op == QCC_PUNCT_SLASH) ? (x / y) : (x % y);
        } else {
            int64_t x = (int64_t)av;
            int64_t y = (int64_t)bv;
            r = (uint64_t)((op == QCC_PUNCT_SLASH) ? (x / y) : (x % y));
        }
        *out = make(r, w, u);
        return QCC_OK;
    }
    case QCC_PUNCT_LT:
    case QCC_PUNCT_GT:
    case QCC_PUNCT_LE:
    case QCC_PUNCT_GE:
    case QCC_PUNCT_EQ_EQ:
    case QCC_PUNCT_BANG_EQ: {
        int r;
        if (u) {
            uint64_t x = av & mask_for(w);
            uint64_t y = bv & mask_for(w);
            switch (op) {
            case QCC_PUNCT_LT:     r = x < y;  break;
            case QCC_PUNCT_GT:     r = x > y;  break;
            case QCC_PUNCT_LE:     r = x <= y; break;
            case QCC_PUNCT_GE:     r = x >= y; break;
            case QCC_PUNCT_EQ_EQ:  r = x == y; break;
            default:               r = x != y; break;
            }
        } else {
            int64_t x = (int64_t)av;
            int64_t y = (int64_t)bv;
            switch (op) {
            case QCC_PUNCT_LT:     r = x < y;  break;
            case QCC_PUNCT_GT:     r = x > y;  break;
            case QCC_PUNCT_LE:     r = x <= y; break;
            case QCC_PUNCT_GE:     r = x >= y; break;
            case QCC_PUNCT_EQ_EQ:  r = x == y; break;
            default:               r = x != y; break;
            }
        }
        *out = make((uint64_t)(r ? 1 : 0), 32, 0); /* a comparison yields int */
        return QCC_OK;
    }
    default:
        return QCC_ERR_TYPE;
    }
}

static qcc_status eval_unary(const qcc_expr *e, ival *out)
{
    ival       a;
    qcc_status st = eval(e->a, &a);
    if (st != QCC_OK) {
        return st;
    }
    a = promote(a);
    switch (e->op) {
    case QCC_PUNCT_PLUS:  *out = a;                                   return QCC_OK;
    case QCC_PUNCT_MINUS: *out = make((uint64_t)0 - a.bits, a.width, a.uns);
        return QCC_OK;
    case QCC_PUNCT_TILDE: *out = make(~a.bits, a.width, a.uns);       return QCC_OK;
    case QCC_PUNCT_BANG:  *out = make((a.bits == 0) ? 1u : 0u, 32, 0); return QCC_OK;
    default:
        /* prefix ++/-- and unary & * are never integer constant expressions. */
        return QCC_ERR_TYPE;
    }
}

static qcc_status eval_binary(const qcc_expr *e, ival *out)
{
    /* Short-circuit logical operators first, so the right operand need not be a
       constant when the result is already decided (§6.6 ¶3). */
    if (e->op == QCC_PUNCT_AMP_AMP || e->op == QCC_PUNCT_PIPE_PIPE) {
        ival       a;
        qcc_status st = eval(e->a, &a);
        if (st != QCC_OK) {
            return st;
        }
        int la = (a.bits != 0);
        if (e->op == QCC_PUNCT_AMP_AMP && !la) {
            *out = make(0, 32, 0);
            return QCC_OK;
        }
        if (e->op == QCC_PUNCT_PIPE_PIPE && la) {
            *out = make(1, 32, 0);
            return QCC_OK;
        }
        ival b;
        st = eval(e->b, &b);
        if (st != QCC_OK) {
            return st;
        }
        int lb = (b.bits != 0);
        int r  = (e->op == QCC_PUNCT_AMP_AMP) ? (la && lb) : (la || lb);
        *out   = make((uint64_t)(r ? 1 : 0), 32, 0);
        return QCC_OK;
    }

    ival       a;
    ival       b;
    qcc_status st = eval(e->a, &a);
    if (st != QCC_OK) {
        return st;
    }
    st = eval(e->b, &b);
    if (st != QCC_OK) {
        return st;
    }
    a = promote(a);
    b = promote(b);

    /* Shifts: the result type is the promoted left operand (§6.5.7); the right
       operand is promoted independently and only supplies the count. */
    if (e->op == QCC_PUNCT_LSHIFT || e->op == QCC_PUNCT_RSHIFT) {
        int      sh = (int)(b.bits & 63u);
        uint64_t r;
        if (e->op == QCC_PUNCT_LSHIFT) {
            r = a.bits << sh;
        } else if (a.uns) {
            r = a.bits >> sh; /* logical right shift for an unsigned operand */
        } else {
            r = (uint64_t)((int64_t)a.bits >> sh); /* arithmetic for signed */
        }
        *out = make(r, a.width, a.uns);
        return QCC_OK;
    }

    return eval_arith_binary(e->op, a, b, out);
}

static qcc_status eval_cast(const qcc_expr *e, ival *out)
{
    const qcc_type *t = e->type_operand;
    if (t == NULL || !qcc_type_is_integer(t)) {
        return QCC_ERR_TYPE; /* a cast to pointer/floating is not an integer ICE. */
    }
    ival       a;
    qcc_status st = eval(e->a, &a);
    if (st != QCC_OK) {
        return st;
    }
    if (t->kind == QCC_TYPE_BOOL) {
        *out = make((a.bits != 0) ? 1u : 0u, 8, 1); /* (_Bool) yields 0 or 1. */
        return QCC_OK;
    }
    uint64_t sz = qcc_type_size(t);
    if (sz == 0 || sz > 8) {
        return QCC_ERR_TYPE;
    }
    int width = (int)(sz * 8u);
    int uns   = qcc_type_is_unsigned_integer(t); /* plain char and enum are signed */
    *out = make(a.bits, width, uns);
    return QCC_OK;
}

static qcc_status eval(const qcc_expr *e, ival *out)
{
    switch (e->kind) {
    case QCC_EXPR_INT_CONST: {
        int w, u;
        int_type_wu(e->tok.int_type, &w, &u);
        *out = make(e->tok.int_value, w, u);
        return QCC_OK;
    }
    case QCC_EXPR_CHAR_CONST:
        /* An integer character constant has type int (§6.4.4.4 ¶10); the wide
           encodings are char16_t/char32_t/wchar_t — modeled by width/signedness. */
        switch (e->tok.char_encoding) {
        case QCC_ENC_CHAR16: *out = make(e->tok.int_value, 16, 1); return QCC_OK;
        case QCC_ENC_CHAR32: *out = make(e->tok.int_value, 32, 1); return QCC_OK;
        default:             *out = make(e->tok.int_value, 32, 0); return QCC_OK;
        }
    case QCC_EXPR_UNARY:
        return eval_unary(e, out);
    case QCC_EXPR_BINARY:
        return eval_binary(e, out);
    case QCC_EXPR_CONDITIONAL: {
        ival       c;
        qcc_status st = eval(e->a, &c);
        if (st != QCC_OK) {
            return st;
        }
        c = promote(c);
        /* Only the selected arm is evaluated, so the other need not be constant. */
        return eval((c.bits != 0) ? e->b : e->c, out);
    }
    case QCC_EXPR_CAST:
        return eval_cast(e, out);
    case QCC_EXPR_SIZEOF_TYPE: {
        uint64_t sz = qcc_type_size(e->type_operand);
        if (sz == 0) {
            return QCC_ERR_TYPE; /* sizeof of void / a function / an incomplete type */
        }
        *out = make(sz, 64, 1); /* size_t is unsigned long in LP64 */
        return QCC_OK;
    }
    case QCC_EXPR_ALIGNOF_TYPE: {
        uint64_t al = qcc_type_align(e->type_operand);
        if (al == 0) {
            return QCC_ERR_TYPE;
        }
        *out = make(al, 64, 1);
        return QCC_OK;
    }
    default:
        /* Identifiers (an enum constant needs the symbol table), `sizeof` of an
           expression (needs its type), floating/string constants, calls,
           subscripts, members, postfix ++/--, assignment, and the comma operator
           are not integer constant expressions this module evaluates. */
        return QCC_ERR_TYPE;
    }
}

qcc_status qcc_eval_const_int(const qcc_expr *e, qcc_const_value *out)
{
    if (e == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    ival       v;
    qcc_status st = eval(e, &v);
    if (st != QCC_OK) {
        return st;
    }
    v          = promote(v); /* report a sub-int result as int (§6.3.1.1). */
    out->value = v.bits;
    out->type  = wu_int_type(v.width, v.uns);
    return QCC_OK;
}
