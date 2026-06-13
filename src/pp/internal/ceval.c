/*
 * qcc — preprocessor internals: #if controlling-expression evaluation (impl).
 *
 * See ceval.h for the four-step pipeline. After `defined` substitution, macro
 * expansion, and identifier->0 replacement, a recursive-descent / precedence-
 * climbing evaluator computes the value. Every value is a `cval` — a magnitude
 * plus a signedness flag — and all arithmetic is done in uintmax_t (two's
 * complement, so signed +,-,*,bitops never invoke undefined overflow), with
 * signedness consulted only where it changes the result (/ % >> and the
 * relational/equality operators), per §6.10.1 ¶4 and §6.6.
 *
 * Short-circuit evaluation of &&, ||, and ?: is implemented with an `eval`
 * flag threaded through the parser: the dead side is still parsed (to consume
 * its tokens) but neither computed nor diagnosed, so `1 || (1/0)` is valid.
 */
#include "pp/internal/ceval.h"

#include <stdint.h>
#include <string.h>

#include "diag/diag.h"
#include "pp/internal/expand.h"
#include "pp/internal/macro.h"

/* An evaluated value: bits in `u`, interpreted as intmax_t when !is_unsigned. */
typedef struct cval {
    uintmax_t u;
    int       is_unsigned;
} cval;

/* Parser cursor over the prepared token list. */
typedef struct eval_ctx {
    qcc_pp         *pp;
    const qcc_ptok *toks;
    size_t          count;
    size_t          pos;
    const qcc_ptok *anchor;  /* For end-of-input diagnostics.                   */
    int             error;   /* Set once a syntax/semantic error is reported.   */
} eval_ctx;

/* Forward declarations (the grammar recurses). */
static cval parse_conditional(eval_ctx *cx, int eval);

/* Private helpers: diagnostics, cursor. */

static void eval_error(eval_ctx *cx, const qcc_ptok *at, const char *msg)
{
    if (cx->error) {
        return; /* Report only the first error per expression. */
    }
    cx->error = 1;
    const qcc_ptok *t = (at != NULL) ? at : cx->anchor;
    const qcc_source *src = (t != NULL) ? t->source : NULL;
    size_t off = (t != NULL) ? t->offset : 0;
    size_t len = (t != NULL && t->spelling_len != 0) ? t->spelling_len : 1u;
    qcc_diag_emit(cx->pp->diags, QCC_DIAG_ERROR, src, off, len, "%s", msg);
}

static const qcc_ptok *peek(eval_ctx *cx)
{
    return (cx->pos < cx->count) ? &cx->toks[cx->pos] : NULL;
}

static const qcc_ptok *advance(eval_ctx *cx)
{
    return (cx->pos < cx->count) ? &cx->toks[cx->pos++] : NULL;
}

static int peek_is_punct(eval_ctx *cx, qcc_punct p)
{
    const qcc_ptok *t = peek(cx);
    return t != NULL && t->kind == QCC_PP_TOKEN_PUNCT && t->punct == p;
}

static int truth(cval v) { return v.u != 0; }

static cval make_int(uintmax_t u, int is_unsigned)
{
    cval v;
    v.u           = u;
    v.is_unsigned = is_unsigned ? 1 : 0;
    return v;
}

/* Parse an integer constant from a pp-number spelling (§6.4.4.1). A pp-number
   that is actually a floating constant is rejected by the caller's check. */
static cval parse_int_literal(eval_ctx *cx, const qcc_ptok *t)
{
    const char *s   = t->spelling;
    size_t      len = t->spelling_len;
    size_t      i   = 0;

    int      base   = 10;
    uintmax_t value = 0;
    int      overflow = 0;

    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i    = 2;
        if (i >= len) {
            eval_error(cx, t, "invalid hexadecimal constant");
            return make_int(0, 0);
        }
    } else if (len >= 1 && s[0] == '0') {
        base = 8; /* Octal (a lone "0" is octal zero). */
        i    = 1;
    }

    size_t digits_start = i;
    for (; i < len; ++i) {
        char c = s[i];
        int  d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = 10 + (c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = 10 + (c - 'A');
        } else {
            break; /* Start of the suffix. */
        }
        if (base == 8 && d >= 8) {
            eval_error(cx, t, "invalid digit in octal constant");
            return make_int(0, 0);
        }
        uintmax_t next = value * (uintmax_t)base + (uintmax_t)d;
        if (next < value) {
            overflow = 1;
        }
        value = next;
    }
    if (i == digits_start && base != 8) {
        eval_error(cx, t, "invalid integer constant");
        return make_int(0, 0);
    }

    /* Suffix: any combination of u/U and l/L (1-2). Only unsignedness matters
       (every value is widened to (u)intmax_t here). */
    int is_unsigned = 0;
    int u_count = 0, l_count = 0;
    for (; i < len; ++i) {
        char c = s[i];
        if (c == 'u' || c == 'U') {
            u_count++;
        } else if (c == 'l' || c == 'L') {
            l_count++;
        } else {
            eval_error(cx, t, "invalid suffix on integer constant");
            return make_int(0, 0);
        }
    }
    if (u_count > 1 || l_count > 2) {
        eval_error(cx, t, "invalid suffix on integer constant");
        return make_int(0, 0);
    }
    if (u_count == 1) {
        is_unsigned = 1;
    }

    if (overflow) {
        eval_error(cx, t, "integer constant is too large for the preprocessor");
        return make_int(0, 0);
    }
    /* A value above INTMAX_MAX cannot be signed intmax_t; it is unsigned. */
    if (value > (uintmax_t)INTMAX_MAX) {
        is_unsigned = 1;
    }
    return make_int(value, is_unsigned);
}

/* Parse a character-constant spelling (§6.4.4.4): value is signed. Prefixes
   (L/u/U) are accepted; the value is the (last) character's code. */
static cval parse_char_literal(eval_ctx *cx, const qcc_ptok *t)
{
    const char *s   = t->spelling;
    size_t      len = t->spelling_len;
    size_t      i   = 0;

    while (i < len && s[i] != '\'') {
        i += 1; /* Skip an L / u / U prefix. */
    }
    if (i >= len) {
        eval_error(cx, t, "invalid character constant");
        return make_int(0, 0);
    }
    i += 1; /* Past the opening quote. */

    uintmax_t value     = 0;
    int       any       = 0;
    while (i < len && s[i] != '\'') {
        unsigned int ch;
        if (s[i] == '\\' && i + 1 < len) {
            i += 1;
            char e = s[i++];
            switch (e) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case '0': ch = '\0'; break;
            case '\\': ch = '\\'; break;
            case '\'': ch = '\''; break;
            case '"': ch = '"'; break;
            case 'a': ch = 7; break;
            case 'b': ch = 8; break;
            case 'f': ch = 12; break;
            case 'v': ch = 11; break;
            case 'x': {
                unsigned int hv = 0;
                int got = 0;
                while (i < len) {
                    char c = s[i];
                    int  d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                    else break;
                    hv = hv * 16u + (unsigned)d;
                    i += 1;
                    got = 1;
                }
                if (!got) {
                    eval_error(cx, t, "\\x used with no hex digits");
                }
                ch = hv & 0xffu;
                break;
            }
            default:
                if (e >= '1' && e <= '7') {
                    unsigned int ov = (unsigned)(e - '0');
                    int n = 1;
                    while (i < len && n < 3 && s[i] >= '0' && s[i] <= '7') {
                        ov = ov * 8u + (unsigned)(s[i] - '0');
                        i += 1;
                        n += 1;
                    }
                    ch = ov & 0xffu;
                } else {
                    ch = (unsigned char)e; /* Unknown escape: the char itself. */
                }
                break;
            }
        } else {
            ch = (unsigned char)s[i++];
        }
        /* Multi-character constants pack bytes (implementation-defined, §6.4.4.4
           ¶10); we mirror the common big-endian packing. */
        value = (value << 8) | (ch & 0xffu);
        any   = 1;
    }
    if (!any) {
        eval_error(cx, t, "empty character constant");
    }
    /* A single char fits a signed int and is positive for ASCII; keep it signed
       so e.g. '\xff' compares as a small positive here (we use the byte value). */
    return make_int(value, 0);
}

static cval parse_primary(eval_ctx *cx, int eval)
{
    const qcc_ptok *t = peek(cx);
    if (t == NULL) {
        eval_error(cx, NULL, "expected value in preprocessor expression");
        return make_int(0, 0);
    }

    if (t->kind == QCC_PP_TOKEN_PUNCT && t->punct == QCC_PUNCT_LPAREN) {
        advance(cx);
        cval v = parse_conditional(cx, eval);
        if (!peek_is_punct(cx, QCC_PUNCT_RPAREN)) {
            eval_error(cx, peek(cx), "expected ')' in preprocessor expression");
        } else {
            advance(cx);
        }
        return v;
    }
    if (t->kind == QCC_PP_TOKEN_PP_NUMBER) {
        advance(cx);
        /* Reject floating constants (§6.10.1 ¶4: integer constants only). */
        for (size_t k = 0; k < t->spelling_len; ++k) {
            char c = t->spelling[k];
            if (c == '.' ||
                ((c == 'e' || c == 'E') && t->spelling_len >= 1 && t->spelling[0] != '0') ||
                c == 'p' || c == 'P') {
                eval_error(cx, t, "floating constant in preprocessor expression");
                return make_int(0, 0);
            }
        }
        return parse_int_literal(cx, t);
    }
    if (t->kind == QCC_PP_TOKEN_CHAR_CONST) {
        advance(cx);
        return parse_char_literal(cx, t);
    }

    eval_error(cx, t, "token is not valid in a preprocessor expression");
    advance(cx);
    return make_int(0, 0);
}

static cval parse_unary(eval_ctx *cx, int eval)
{
    const qcc_ptok *t = peek(cx);
    if (t != NULL && t->kind == QCC_PP_TOKEN_PUNCT) {
        if (t->punct == QCC_PUNCT_PLUS) {
            advance(cx);
            return parse_unary(cx, eval);
        }
        if (t->punct == QCC_PUNCT_MINUS) {
            advance(cx);
            cval v = parse_unary(cx, eval);
            return make_int((uintmax_t)0 - v.u, v.is_unsigned);
        }
        if (t->punct == QCC_PUNCT_TILDE) {
            advance(cx);
            cval v = parse_unary(cx, eval);
            return make_int(~v.u, v.is_unsigned);
        }
        if (t->punct == QCC_PUNCT_BANG) {
            advance(cx);
            cval v = parse_unary(cx, eval);
            return make_int(truth(v) ? 0u : 1u, 0);
        }
    }
    return parse_primary(cx, eval);
}

/* Binary precedence for the operators between `|` (lowest) and `*` (highest);
   -1 for anything that is not one of those binary operators. && || ?: are
   handled at a lower level by the dedicated functions below. */
static int binop_prec(const qcc_ptok *t)
{
    if (t == NULL || t->kind != QCC_PP_TOKEN_PUNCT) {
        return -1;
    }
    switch (t->punct) {
    case QCC_PUNCT_STAR: case QCC_PUNCT_SLASH: case QCC_PUNCT_PERCENT: return 10;
    case QCC_PUNCT_PLUS: case QCC_PUNCT_MINUS:                         return 9;
    case QCC_PUNCT_LSHIFT: case QCC_PUNCT_RSHIFT:                      return 8;
    case QCC_PUNCT_LT: case QCC_PUNCT_GT:
    case QCC_PUNCT_LE: case QCC_PUNCT_GE:                             return 7;
    case QCC_PUNCT_EQ_EQ: case QCC_PUNCT_BANG_EQ:                     return 6;
    case QCC_PUNCT_AMP:                                              return 5;
    case QCC_PUNCT_CARET:                                            return 4;
    case QCC_PUNCT_PIPE:                                             return 3;
    default:                                                         return -1;
    }
}

/* Apply a binary operator. Reports (once) on division by zero or a bad shift. */
static cval apply_binop(eval_ctx *cx, qcc_punct op, cval a, cval b, int eval,
                        const qcc_ptok *at)
{
    int ru = a.is_unsigned || b.is_unsigned; /* Usual-conversions signedness.    */

    switch (op) {
    case QCC_PUNCT_STAR:  return make_int(a.u * b.u, ru);
    case QCC_PUNCT_PLUS:  return make_int(a.u + b.u, ru);
    case QCC_PUNCT_MINUS: return make_int(a.u - b.u, ru);
    case QCC_PUNCT_SLASH:
    case QCC_PUNCT_PERCENT:
        if (!eval) {
            return make_int(0, ru);
        }
        if (b.u == 0) {
            eval_error(cx, at, "division by zero in preprocessor expression");
            return make_int(0, ru);
        }
        if (ru) {
            return make_int(op == QCC_PUNCT_SLASH ? a.u / b.u : a.u % b.u, 1);
        } else {
            intmax_t ia = (intmax_t)a.u, ib = (intmax_t)b.u;
            if (ib == -1) { /* Avoid INTMAX_MIN/-1 overflow (UB). */
                return make_int(op == QCC_PUNCT_SLASH ? (uintmax_t)0 - a.u : 0u, 0);
            }
            return make_int((uintmax_t)(op == QCC_PUNCT_SLASH ? ia / ib : ia % ib), 0);
        }
    case QCC_PUNCT_LSHIFT:
    case QCC_PUNCT_RSHIFT: {
        int shift_unsigned = a.is_unsigned; /* Result type is the left type.     */
        if (eval) {
            intmax_t cnt = (intmax_t)b.u;
            if (cnt < 0 || cnt >= (intmax_t)(sizeof(uintmax_t) * 8)) {
                eval_error(cx, at, "shift count out of range");
                return make_int(0, shift_unsigned);
            }
        }
        unsigned sh = (unsigned)(b.u & (sizeof(uintmax_t) * 8 - 1));
        if (op == QCC_PUNCT_LSHIFT) {
            return make_int(a.u << sh, shift_unsigned);
        }
        if (shift_unsigned) {
            return make_int(a.u >> sh, 1); /* Logical. */
        }
        return make_int((uintmax_t)((intmax_t)a.u >> sh), 0); /* Arithmetic. */
    }
    case QCC_PUNCT_LT: case QCC_PUNCT_GT: case QCC_PUNCT_LE: case QCC_PUNCT_GE: {
        int r;
        if (ru) {
            r = (op == QCC_PUNCT_LT) ? (a.u <  b.u) :
                (op == QCC_PUNCT_GT) ? (a.u >  b.u) :
                (op == QCC_PUNCT_LE) ? (a.u <= b.u) : (a.u >= b.u);
        } else {
            intmax_t ia = (intmax_t)a.u, ib = (intmax_t)b.u;
            r = (op == QCC_PUNCT_LT) ? (ia <  ib) :
                (op == QCC_PUNCT_GT) ? (ia >  ib) :
                (op == QCC_PUNCT_LE) ? (ia <= ib) : (ia >= ib);
        }
        return make_int(r ? 1u : 0u, 0);
    }
    case QCC_PUNCT_EQ_EQ:   return make_int(a.u == b.u ? 1u : 0u, 0);
    case QCC_PUNCT_BANG_EQ: return make_int(a.u != b.u ? 1u : 0u, 0);
    case QCC_PUNCT_AMP:     return make_int(a.u & b.u, ru);
    case QCC_PUNCT_CARET:   return make_int(a.u ^ b.u, ru);
    case QCC_PUNCT_PIPE:    return make_int(a.u | b.u, ru);
    default:                return make_int(0, 0); /* Unreachable. */
    }
}

/* Precedence climbing over the binary operators | … * (above unary). */
static cval parse_binary(eval_ctx *cx, int min_prec, int eval)
{
    cval left = parse_unary(cx, eval);
    for (;;) {
        const qcc_ptok *t    = peek(cx);
        int             prec = binop_prec(t);
        if (prec < 0 || prec < min_prec) {
            break;
        }
        qcc_punct       op = t->punct;
        const qcc_ptok *at = t;
        advance(cx);
        cval right = parse_binary(cx, prec + 1, eval);
        left = apply_binop(cx, op, left, right, eval, at);
    }
    return left;
}

static cval parse_logical_and(eval_ctx *cx, int eval)
{
    cval left = parse_binary(cx, 3, eval);
    while (peek_is_punct(cx, QCC_PUNCT_AMP_AMP)) {
        advance(cx);
        cval right = parse_binary(cx, 3, eval && truth(left));
        left = make_int((truth(left) && truth(right)) ? 1u : 0u, 0);
    }
    return left;
}

static cval parse_logical_or(eval_ctx *cx, int eval)
{
    cval left = parse_logical_and(cx, eval);
    while (peek_is_punct(cx, QCC_PUNCT_PIPE_PIPE)) {
        advance(cx);
        cval right = parse_logical_and(cx, eval && !truth(left));
        left = make_int((truth(left) || truth(right)) ? 1u : 0u, 0);
    }
    return left;
}

static cval parse_conditional(eval_ctx *cx, int eval)
{
    cval cond = parse_logical_or(cx, eval);
    if (!peek_is_punct(cx, QCC_PUNCT_QUESTION)) {
        return cond;
    }
    advance(cx);
    int take = truth(cond);
    cval a = parse_conditional(cx, eval && take);
    if (!peek_is_punct(cx, QCC_PUNCT_COLON)) {
        eval_error(cx, peek(cx), "expected ':' in preprocessor conditional");
        return make_int(0, 0);
    }
    advance(cx);
    cval b = parse_conditional(cx, eval && !take);
    cval r = take ? a : b;
    r.is_unsigned = a.is_unsigned || b.is_unsigned; /* Usual conversions. */
    return r;
}

/* Pipeline step 1: replace `defined X` / `defined ( X )` by 1 or 0. */
static qcc_status substitute_defined(qcc_pp *pp, const qcc_ptok *toks,
                                     size_t count, qcc_ptok_list *out)
{
    size_t i = 0;
    while (i < count) {
        const qcc_ptok *t = &toks[i];
        if (t->kind == QCC_PP_TOKEN_IDENTIFIER &&
            strcmp(t->spelling, "defined") == 0) {
            const qcc_ptok *anchor = t;
            i += 1;
            int has_paren = 0;
            if (i < count && toks[i].kind == QCC_PP_TOKEN_PUNCT &&
                toks[i].punct == QCC_PUNCT_LPAREN) {
                has_paren = 1;
                i += 1;
            }
            int defined = 0;
            if (i < count && toks[i].kind == QCC_PP_TOKEN_IDENTIFIER) {
                defined = (qcc_macro_lookup(pp->macros, toks[i].spelling) != NULL);
                i += 1;
                if (has_paren) {
                    if (i < count && toks[i].kind == QCC_PP_TOKEN_PUNCT &&
                        toks[i].punct == QCC_PUNCT_RPAREN) {
                        i += 1;
                    } else {
                        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, anchor->source,
                                      anchor->offset, 7,
                                      "missing ')' after 'defined'");
                    }
                }
            } else {
                qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, anchor->source,
                              anchor->offset, 7,
                              "operator 'defined' requires an identifier");
            }

            qcc_ptok num;
            memset(&num, 0, sizeof(num));
            num.kind         = QCC_PP_TOKEN_PP_NUMBER;
            num.spelling     = qcc_pp_intern(pp, defined ? "1" : "0", 1);
            num.spelling_len = 1;
            num.source       = anchor->source;
            num.offset       = anchor->offset;
            num.line         = anchor->line;
            num.column       = anchor->column;
            num.leading_space = anchor->leading_space;
            if (num.spelling == NULL) {
                return QCC_ERR_OUT_OF_MEMORY;
            }
            qcc_status st = qcc_ptok_list_push(out, &num);
            if (st != QCC_OK) {
                return st;
            }
        } else {
            qcc_status st = qcc_ptok_list_push(out, t);
            if (st != QCC_OK) {
                return st;
            }
            i += 1;
        }
    }
    return QCC_OK;
}

/* Pipeline step 3: turn every surviving identifier into the pp-number 0. */
static qcc_status identifiers_to_zero(qcc_pp *pp, qcc_ptok_list *list)
{
    const char *zero = qcc_pp_intern(pp, "0", 1);
    if (zero == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].kind == QCC_PP_TOKEN_IDENTIFIER) {
            list->items[i].kind         = QCC_PP_TOKEN_PP_NUMBER;
            list->items[i].punct        = (qcc_punct)0;
            list->items[i].spelling     = zero;
            list->items[i].spelling_len = 1;
        }
    }
    return QCC_OK;
}

qcc_status qcc_pp_eval_condition(qcc_pp *pp, const qcc_ptok *toks, size_t count,
                                 int *out_true)
{
    if (pp == NULL || out_true == NULL || (toks == NULL && count != 0)) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out_true = 0;

    qcc_ptok_list defined_done;
    qcc_ptok_list_init(&defined_done);
    qcc_status st = substitute_defined(pp, toks, count, &defined_done);

    qcc_ptok_list expanded;
    qcc_ptok_list_init(&expanded);
    if (st == QCC_OK) {
        st = qcc_pp_expand_all(pp, defined_done.items, defined_done.count, &expanded);
    }
    if (st == QCC_OK) {
        st = identifiers_to_zero(pp, &expanded);
    }

    if (st == QCC_OK) {
        if (expanded.count == 0) {
            const qcc_source *src = (count > 0) ? toks[0].source : NULL;
            size_t off = (count > 0) ? toks[0].offset : 0;
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, src, off, 1,
                          "#if with no expression");
        } else {
            eval_ctx cx;
            cx.pp     = pp;
            cx.toks   = expanded.items;
            cx.count  = expanded.count;
            cx.pos    = 0;
            cx.anchor = &expanded.items[0];
            cx.error  = 0;

            cval v = parse_conditional(&cx, 1);
            if (!cx.error && cx.pos != cx.count) {
                eval_error(&cx, peek(&cx),
                           "extra tokens after preprocessor expression");
            }
            if (!cx.error) {
                *out_true = truth(v) ? 1 : 0;
            }
        }
    }

    qcc_ptok_list_dispose(&expanded);
    qcc_ptok_list_dispose(&defined_done);
    return st;
}
