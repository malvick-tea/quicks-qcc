/*
 * qcc — the parser (implementation).
 *
 * See parser.h and ADR-0019. The expression grammar is parsed by recursive
 * descent, with the fifteen binary-operator levels (§6.5.5-6.5.14) collapsed into
 * one precedence-climbing loop driven by binop_prec(). Each grammar function
 * returns the node it built, or NULL on either a syntax error (a diagnostic has
 * been emitted, had_error set) or a node-allocation failure (oom set); the top
 * level distinguishes the two for its status. The cursor never advances past the
 * terminating EOF token.
 */
#include "parser/parser.h"

#include <stdlib.h>

qcc_status qcc_parser_init(qcc_parser *parser, const qcc_token *tokens,
                           size_t count, qcc_ast *ast, qcc_diag_sink *diags)
{
    if (parser == NULL || tokens == NULL || count == 0 || ast == NULL ||
        diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    parser->tokens    = tokens;
    parser->count     = count;
    parser->pos       = 0;
    parser->ast       = ast;
    parser->diags     = diags;
    parser->had_error = 0;
    parser->oom       = 0;
    return QCC_OK;
}

/* Cursor primitives. */

static const qcc_token *peek(const qcc_parser *p)
{
    return &p->tokens[p->pos];
}

static void advance(qcc_parser *p)
{
    if (p->tokens[p->pos].kind != QCC_TOKEN_EOF && p->pos + 1 < p->count) {
        ++p->pos;
    }
}

int qcc_parser_at_end(const qcc_parser *p)
{
    return peek(p)->kind == QCC_TOKEN_EOF;
}

static int at_punct(const qcc_parser *p, qcc_punct punct)
{
    const qcc_token *t = peek(p);
    return t->kind == QCC_TOKEN_PUNCT && t->punct == punct;
}

/* Underline width for a token in diagnostics (at least one column). */
static size_t tok_span(const qcc_token *t)
{
    return (t->spelling_len != 0) ? t->spelling_len : 1u;
}

/* Record a syntax error at token `t` and mark the parse failed. */
static void parse_error(qcc_parser *p, const qcc_token *t, const char *msg)
{
    qcc_status st = qcc_diag_emit(p->diags, QCC_DIAG_ERROR, t->source, t->offset,
                                  tok_span(t), "%s", msg);
    if (st != QCC_OK) {
        p->oom = 1;
    }
    p->had_error = 1;
}

/* Mark an allocation failure and return NULL (a node constructor gave NULL). */
static qcc_expr *fail_oom(qcc_parser *p)
{
    p->oom = 1;
    return NULL;
}

/* Consume `punct` if present; otherwise diagnose with `msg`. Returns 1/0. */
static int expect_punct(qcc_parser *p, qcc_punct punct, const char *msg)
{
    if (at_punct(p, punct)) {
        advance(p);
        return 1;
    }
    parse_error(p, peek(p), msg);
    return 0;
}

/* Operator classification (§6.5). */

/* Binding precedence of a binary operator (§6.5.5-6.5.14); 0 if `punct` is not
   one. Higher binds tighter; all are left-associative. */
static int binop_prec(qcc_punct punct)
{
    switch (punct) {
    case QCC_PUNCT_STAR:
    case QCC_PUNCT_SLASH:
    case QCC_PUNCT_PERCENT:  return 10;
    case QCC_PUNCT_PLUS:
    case QCC_PUNCT_MINUS:    return 9;
    case QCC_PUNCT_LSHIFT:
    case QCC_PUNCT_RSHIFT:   return 8;
    case QCC_PUNCT_LT:
    case QCC_PUNCT_GT:
    case QCC_PUNCT_LE:
    case QCC_PUNCT_GE:       return 7;
    case QCC_PUNCT_EQ_EQ:
    case QCC_PUNCT_BANG_EQ:  return 6;
    case QCC_PUNCT_AMP:      return 5;
    case QCC_PUNCT_CARET:    return 4;
    case QCC_PUNCT_PIPE:     return 3;
    case QCC_PUNCT_AMP_AMP:  return 2;
    case QCC_PUNCT_PIPE_PIPE: return 1;
    default:                 return 0;
    }
}

/* Is `punct` a prefix unary operator (§6.5.3)? */
static int is_unary_prefix(qcc_punct punct)
{
    switch (punct) {
    case QCC_PUNCT_PLUS_PLUS:
    case QCC_PUNCT_MINUS_MINUS:
    case QCC_PUNCT_AMP:
    case QCC_PUNCT_STAR:
    case QCC_PUNCT_PLUS:
    case QCC_PUNCT_MINUS:
    case QCC_PUNCT_TILDE:
    case QCC_PUNCT_BANG:     return 1;
    default:                 return 0;
    }
}

/* Is the current token an assignment operator (§6.5.16)? If so, write it. */
static int cur_assign_op(const qcc_parser *p, qcc_punct *out)
{
    const qcc_token *t = peek(p);
    if (t->kind != QCC_TOKEN_PUNCT) {
        return 0;
    }
    switch (t->punct) {
    case QCC_PUNCT_EQ:
    case QCC_PUNCT_STAR_EQ:
    case QCC_PUNCT_SLASH_EQ:
    case QCC_PUNCT_PERCENT_EQ:
    case QCC_PUNCT_PLUS_EQ:
    case QCC_PUNCT_MINUS_EQ:
    case QCC_PUNCT_LSHIFT_EQ:
    case QCC_PUNCT_RSHIFT_EQ:
    case QCC_PUNCT_AMP_EQ:
    case QCC_PUNCT_CARET_EQ:
    case QCC_PUNCT_PIPE_EQ:
        *out = t->punct;
        return 1;
    default:
        return 0;
    }
}

/* The expression grammar (§6.5). Mutually recursive; forward-declared. */
static qcc_expr *parse_expression(qcc_parser *p);
static qcc_expr *parse_assignment(qcc_parser *p);
static qcc_expr *parse_unary(qcc_parser *p);

/* primary-expression (§6.5.1): identifier, constant, string, or ( expression ). */
static qcc_expr *parse_primary(qcc_parser *p)
{
    const qcc_token *t = peek(p);
    switch (t->kind) {
    case QCC_TOKEN_IDENTIFIER:
    case QCC_TOKEN_INTEGER:
    case QCC_TOKEN_FLOATING:
    case QCC_TOKEN_CHAR:
    case QCC_TOKEN_STRING: {
        advance(p);
        qcc_expr *e = qcc_expr_leaf(p->ast, t);
        return (e != NULL) ? e : fail_oom(p);
    }
    case QCC_TOKEN_PUNCT:
        if (t->punct == QCC_PUNCT_LPAREN) {
            advance(p);
            qcc_expr *e = parse_expression(p);
            if (e == NULL) {
                return NULL;
            }
            if (!expect_punct(p, QCC_PUNCT_RPAREN,
                              "expected ')' after parenthesized expression")) {
                return NULL;
            }
            return e;
        }
        break;
    default:
        break;
    }
    parse_error(p, t, "expected an expression");
    return NULL;
}

/* The argument list of a call (§6.5.2.2), the '(' already consumed. */
static qcc_expr *parse_call(qcc_parser *p, qcc_expr *callee,
                            const qcc_token *loc)
{
    qcc_expr **args = NULL;
    size_t     n    = 0;
    size_t     cap  = 0;

    if (!at_punct(p, QCC_PUNCT_RPAREN)) {
        for (;;) {
            qcc_expr *arg = parse_assignment(p);
            if (arg == NULL) {
                free(args);
                return NULL;
            }
            if (n == cap) {
                size_t     ncap  = (cap == 0) ? 4u : cap * 2u;
                qcc_expr **grown = (qcc_expr **)realloc(args,
                                                        ncap * sizeof(*grown));
                if (grown == NULL) {
                    free(args);
                    return fail_oom(p);
                }
                args = grown;
                cap  = ncap;
            }
            args[n++] = arg;
            if (!at_punct(p, QCC_PUNCT_COMMA)) {
                break;
            }
            advance(p); /* the comma */
        }
    }

    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after arguments")) {
        free(args);
        return NULL;
    }
    qcc_expr *call = qcc_expr_call(p->ast, callee, args, n, loc);
    free(args);
    return (call != NULL) ? call : fail_oom(p);
}

/* postfix-expression (§6.5.2): a primary followed by [],  (), ./->, ++/--. */
static qcc_expr *parse_postfix(qcc_parser *p)
{
    qcc_expr *e = parse_primary(p);
    if (e == NULL) {
        return NULL;
    }

    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind != QCC_TOKEN_PUNCT) {
            return e;
        }
        if (t->punct == QCC_PUNCT_LBRACKET) {
            advance(p);
            qcc_expr *idx = parse_expression(p);
            if (idx == NULL) {
                return NULL;
            }
            if (!expect_punct(p, QCC_PUNCT_RBRACKET,
                              "expected ']' after array subscript")) {
                return NULL;
            }
            e = qcc_expr_index(p->ast, e, idx, t);
        } else if (t->punct == QCC_PUNCT_LPAREN) {
            advance(p);
            e = parse_call(p, e, t);
        } else if (t->punct == QCC_PUNCT_DOT || t->punct == QCC_PUNCT_ARROW) {
            int is_arrow = (t->punct == QCC_PUNCT_ARROW);
            advance(p);
            const qcc_token *name = peek(p);
            if (name->kind != QCC_TOKEN_IDENTIFIER) {
                parse_error(p, name, "expected a member name after '.' or '->'");
                return NULL;
            }
            advance(p);
            e = qcc_expr_member(p->ast, e, is_arrow, name->spelling,
                                name->spelling_len, t);
        } else if (t->punct == QCC_PUNCT_PLUS_PLUS ||
                   t->punct == QCC_PUNCT_MINUS_MINUS) {
            advance(p);
            e = qcc_expr_postfix(p->ast, t->punct, e, t);
        } else {
            return e;
        }
        if (e == NULL) {
            return fail_oom(p);
        }
    }
}

/* unary-expression (§6.5.3): prefix ++/--, & * + - ~ !, or sizeof expr. */
static qcc_expr *parse_unary(qcc_parser *p)
{
    const qcc_token *t = peek(p);

    if (t->kind == QCC_TOKEN_PUNCT && is_unary_prefix(t->punct)) {
        advance(p);
        qcc_expr *operand = parse_unary(p);
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_unary(p->ast, t->punct, operand, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    if (t->kind == QCC_TOKEN_KEYWORD && t->keyword == QCC_KW_SIZEOF) {
        /* sizeof(type-name) needs the type parser (Unit 2); here sizeof always
           takes a unary-expression, so sizeof(x) reads as sizeof of (x). */
        advance(p);
        qcc_expr *operand = parse_unary(p);
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_sizeof(p->ast, operand, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    return parse_postfix(p);
}

/* The binary-operator cascade (§6.5.5-6.5.14) by precedence climbing: parse a
   unary operand, then fold in operators of precedence >= min_prec, recursing one
   level tighter for the right operand (left associativity). */
static qcc_expr *parse_binary(qcc_parser *p, int min_prec)
{
    qcc_expr *lhs = parse_unary(p);
    if (lhs == NULL) {
        return NULL;
    }

    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind != QCC_TOKEN_PUNCT) {
            return lhs;
        }
        int prec = binop_prec(t->punct);
        if (prec == 0 || prec < min_prec) {
            return lhs;
        }
        qcc_punct op = t->punct;
        advance(p);
        qcc_expr *rhs = parse_binary(p, prec + 1);
        if (rhs == NULL) {
            return NULL;
        }
        lhs = qcc_expr_binary(p->ast, op, lhs, rhs, t);
        if (lhs == NULL) {
            return fail_oom(p);
        }
    }
}

/* conditional-expression (§6.5.15): a logical-OR optionally followed by ?:. */
static qcc_expr *parse_conditional(qcc_parser *p)
{
    qcc_expr *cond = parse_binary(p, 1);
    if (cond == NULL) {
        return NULL;
    }
    if (!at_punct(p, QCC_PUNCT_QUESTION)) {
        return cond;
    }
    const qcc_token *q = peek(p);
    advance(p);

    /* §6.5.15: the middle is a full expression; the else branch is itself a
       conditional-expression (so ?: is right-associative). */
    qcc_expr *then_e = parse_expression(p);
    if (then_e == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_COLON, "expected ':' in conditional expression")) {
        return NULL;
    }
    qcc_expr *else_e = parse_conditional(p);
    if (else_e == NULL) {
        return NULL;
    }
    qcc_expr *e = qcc_expr_conditional(p->ast, cond, then_e, else_e, q);
    return (e != NULL) ? e : fail_oom(p);
}

/* assignment-expression (§6.5.16): a conditional, optionally an assignment whose
   right side is again an assignment-expression (right associativity). */
static qcc_expr *parse_assignment(qcc_parser *p)
{
    qcc_expr *lhs = parse_conditional(p);
    if (lhs == NULL) {
        return NULL;
    }
    qcc_punct op;
    if (!cur_assign_op(p, &op)) {
        return lhs;
    }
    const qcc_token *t = peek(p);
    advance(p);
    qcc_expr *rhs = parse_assignment(p);
    if (rhs == NULL) {
        return NULL;
    }
    qcc_expr *e = qcc_expr_assign(p->ast, op, lhs, rhs, t);
    return (e != NULL) ? e : fail_oom(p);
}

/* expression (§6.5.17): a comma-separated sequence of assignment-expressions. */
static qcc_expr *parse_expression(qcc_parser *p)
{
    qcc_expr *e = parse_assignment(p);
    if (e == NULL) {
        return NULL;
    }
    while (at_punct(p, QCC_PUNCT_COMMA)) {
        const qcc_token *t = peek(p);
        advance(p);
        qcc_expr *rhs = parse_assignment(p);
        if (rhs == NULL) {
            return NULL;
        }
        e = qcc_expr_comma(p->ast, e, rhs, t);
        if (e == NULL) {
            return fail_oom(p);
        }
    }
    return e;
}

qcc_status qcc_parse_expression(qcc_parser *p, qcc_expr **out)
{
    if (p == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;

    qcc_expr *e = parse_expression(p);
    if (p->oom) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    if (e == NULL || p->had_error) {
        return QCC_ERR_PARSE;
    }
    *out = e;
    return QCC_OK;
}
