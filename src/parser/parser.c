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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "consteval/consteval.h"

qcc_status qcc_parser_init(qcc_parser *parser, const qcc_token *tokens,
                           size_t count, qcc_ast *ast, qcc_type_ctx *types,
                           qcc_symtab *syms, qcc_diag_sink *diags)
{
    if (parser == NULL || tokens == NULL || count == 0 || ast == NULL ||
        diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    parser->tokens    = tokens;
    parser->count     = count;
    parser->pos       = 0;
    parser->ast       = ast;
    parser->types     = types; /* May be NULL for expression-only use. */
    parser->syms      = syms;
    parser->diags     = diags;
    parser->had_error = 0;
    parser->oom       = 0;
    parser->capture_params  = 0;
    parser->cap_params      = NULL;
    parser->cap_param_count = 0;
    parser->in_param_list   = 0;
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
static qcc_expr *parse_cast(qcc_parser *p);
static qcc_expr *parse_unary(qcc_parser *p);
static int is_decl_specifier_keyword(qcc_keyword kw);
/* The type-name parser (§6.7.7), defined with the declaration grammar below and
   shared with the cast / sizeof / _Alignof hooks here. NULL on error (had_error
   or oom set). */
static const qcc_type *parse_type_name(qcc_parser *p);

/* Does the current '(' open a type-name (a cast, or sizeof/_Alignof operand)?
   True only when a type context exists and the token after '(' begins a
   type-name — a declaration-specifier keyword or a visible typedef-name. */
static int lparen_starts_type(const qcc_parser *p)
{
    if (p->types == NULL || !at_punct(p, QCC_PUNCT_LPAREN) ||
        p->pos + 1 >= p->count) {
        return 0;
    }
    const qcc_token *nx = &p->tokens[p->pos + 1];
    if (nx->kind == QCC_TOKEN_KEYWORD) {
        return is_decl_specifier_keyword(nx->keyword);
    }
    if (nx->kind == QCC_TOKEN_IDENTIFIER) {
        return p->syms != NULL &&
               qcc_symtab_is_typedef_name(p->syms, nx->spelling, nx->spelling_len);
    }
    return 0;
}

/* primary-expression (§6.5.1): identifier, constant, string, or ( expression ). */
static qcc_expr *parse_primary(qcc_parser *p)
{
    const qcc_token *t = peek(p);
    switch (t->kind) {
    case QCC_TOKEN_IDENTIFIER: {
        /* §6.5.1/§6.7.2.2 ¶3: an enumeration constant is a primary expression of
           type int. Fold it to that value here — consteval is pure and cannot
           reach the symbol table (ADR-0026), so resolving the name at the parser,
           which holds the table, lets an enum constant work in every integer
           constant-expression site (array bound, case label, another enumerator). */
        if (p->syms != NULL) {
            const qcc_symbol *sym = qcc_symtab_lookup(
                p->syms, t->spelling, t->spelling_len, QCC_NS_ORDINARY);
            if (sym != NULL && sym->kind == QCC_SYM_ENUM_CONST) {
                advance(p);
                qcc_token lit = *t;          /* keep the name's spelling/location */
                lit.kind      = QCC_TOKEN_INTEGER;
                lit.int_value = (uint64_t)sym->enum_value;
                lit.int_type  = QCC_INT_INT; /* enum constants have type int      */
                qcc_expr *e = qcc_expr_leaf(p->ast, &lit);
                return (e != NULL) ? e : fail_oom(p);
            }
        }
        advance(p);
        qcc_expr *e = qcc_expr_leaf(p->ast, t);
        return (e != NULL) ? e : fail_oom(p);
    }
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

/*
 * unary-expression (§6.5.3). Per the grammar the operands differ by operator:
 * `++`/`--` bind a *unary-expression*, while `& * + - ~ !` bind a
 * *cast-expression* (so `-(int)x` negates the cast). `sizeof` takes either a
 * unary-expression or a parenthesised type-name (§6.5.3.4); `_Alignof` takes only
 * a parenthesised type-name. The type forms are recognised only when a type
 * context exists and lparen_starts_type() confirms the '(' opens a type-name —
 * otherwise `sizeof (x)` is sizeof of a parenthesised expression and '(' is an
 * ordinary primary.
 */
static qcc_expr *parse_unary(qcc_parser *p)
{
    const qcc_token *t = peek(p);

    if (t->kind == QCC_TOKEN_PUNCT &&
        (t->punct == QCC_PUNCT_PLUS_PLUS || t->punct == QCC_PUNCT_MINUS_MINUS)) {
        advance(p);
        qcc_expr *operand = parse_unary(p); /* ++/-- bind a unary-expression. */
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_unary(p->ast, t->punct, operand, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    if (t->kind == QCC_TOKEN_PUNCT && is_unary_prefix(t->punct)) {
        advance(p);
        qcc_expr *operand = parse_cast(p); /* & * + - ~ ! bind a cast-expression. */
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_unary(p->ast, t->punct, operand, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    if (t->kind == QCC_TOKEN_KEYWORD && t->keyword == QCC_KW_SIZEOF) {
        advance(p);
        if (lparen_starts_type(p)) {
            advance(p); /* '(' */
            const qcc_type *type = parse_type_name(p);
            if (type == NULL) {
                return NULL;
            }
            if (!expect_punct(p, QCC_PUNCT_RPAREN,
                              "expected ')' after type name in sizeof")) {
                return NULL;
            }
            qcc_expr *e = qcc_expr_sizeof_type(p->ast, type, t);
            return (e != NULL) ? e : fail_oom(p);
        }
        qcc_expr *operand = parse_unary(p);
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_sizeof(p->ast, operand, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    if (t->kind == QCC_TOKEN_KEYWORD && t->keyword == QCC_KW_ALIGNOF) {
        /* §6.5.3.4: _Alignof has only the `_Alignof ( type-name )` form. */
        advance(p);
        if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after _Alignof")) {
            return NULL;
        }
        const qcc_type *type = parse_type_name(p);
        if (type == NULL) {
            return NULL;
        }
        if (!expect_punct(p, QCC_PUNCT_RPAREN,
                          "expected ')' after type name in _Alignof")) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_alignof_type(p->ast, type, t);
        return (e != NULL) ? e : fail_oom(p);
    }

    return parse_postfix(p);
}

/*
 * cast-expression (§6.5.4): a unary-expression, or `( type-name ) cast-expression`
 * (right-recursive, so `(int)(char)x` casts to char then to int). The leading '('
 * is a cast only when lparen_starts_type() confirms a type context and a type-name
 * token after it; otherwise the '(' falls through to the parenthesised-primary
 * path. A compound literal `( type-name ) { ... }` (§6.5.2.5) shares this prefix
 * but is not supported yet, so it is diagnosed rather than mis-parsed as a cast.
 */
static qcc_expr *parse_cast(qcc_parser *p)
{
    if (lparen_starts_type(p)) {
        const qcc_token *lp = peek(p);
        advance(p); /* '(' */
        const qcc_type *type = parse_type_name(p);
        if (type == NULL) {
            return NULL;
        }
        if (!expect_punct(p, QCC_PUNCT_RPAREN,
                          "expected ')' after type name in cast")) {
            return NULL;
        }
        if (at_punct(p, QCC_PUNCT_LBRACE)) {
            parse_error(p, peek(p), "compound literals are not supported yet");
            return NULL;
        }
        qcc_expr *operand = parse_cast(p);
        if (operand == NULL) {
            return NULL;
        }
        qcc_expr *e = qcc_expr_cast(p->ast, type, operand, lp);
        return (e != NULL) ? e : fail_oom(p);
    }
    return parse_unary(p);
}

/* The binary-operator cascade (§6.5.5-6.5.14) by precedence climbing: parse a
   cast-expression operand (the tightest non-binary level, §6.5.5), then fold in
   operators of precedence >= min_prec, recursing one level tighter for the right
   operand (left associativity). */
static qcc_expr *parse_binary(qcc_parser *p, int min_prec)
{
    qcc_expr *lhs = parse_cast(p);
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

/*
 * Declarations (§6.7). The declaration parser turns declaration-specifiers + a
 * declarator into a qcc_type and a name, building the type in the parser's
 * qcc_type_ctx and registering the name in its qcc_symtab. Declarator binding is
 * inside-out (§6.7.6): pointers prepend, array/function suffixes append, and a
 * parenthesised (nested) declarator is parsed by the standard two-pass method —
 * skip the inner declarator to apply the trailing suffixes to the type, then
 * re-parse the inner declarator against that suffixed type. See ADR-0022.
 */

/* Accumulated declaration-specifiers (§6.7.2): storage class, qualifier and
   function-specifier flags, and a count of each arithmetic type-specifier keyword
   (the keywords are unordered, §6.7.2 ¶2), or a typedef-name / tagged type. */
typedef struct declspec {
    qcc_storage_class storage;
    int               storage_set;
    unsigned          func_spec;
    unsigned          quals;
    int               c_void, c_bool, c_char, c_short, c_int;
    int               c_long, c_signed, c_unsigned, c_float, c_double;
    int               type_specifier_count;
    const qcc_type   *named_type; /* a typedef-name's type or a struct/union/enum */
    int               has_named_type;
} declspec;

static const qcc_type *parse_declarator(qcc_parser *p, const qcc_type *base,
                                        int abstract, const qcc_token **out_name);
static const qcc_type *parse_type_suffix(qcc_parser *p, const qcc_type *type,
                                         int abstract);
static void parse_declaration_specifiers(qcc_parser *p, declspec *ds);
static const qcc_type *ds_base_type(qcc_parser *p, const declspec *ds,
                                    const qcc_token *loc);
static const qcc_type *parse_enum_specifier(qcc_parser *p, const qcc_token *kw);
static const qcc_type *parse_struct_union_specifier(qcc_parser *p,
                                                    qcc_type_kind kind,
                                                    const qcc_token *kw);

static int at_keyword(const qcc_parser *p, qcc_keyword kw)
{
    const qcc_token *t = peek(p);
    return t->kind == QCC_TOKEN_KEYWORD && t->keyword == kw;
}

/* Is `kw` a declaration-specifier keyword (storage/type/qualifier/function)? */
static int is_decl_specifier_keyword(qcc_keyword kw)
{
    switch (kw) {
    case QCC_KW_TYPEDEF: case QCC_KW_EXTERN: case QCC_KW_STATIC:
    case QCC_KW_AUTO:    case QCC_KW_REGISTER: case QCC_KW_THREAD_LOCAL:
    case QCC_KW_CONST:   case QCC_KW_VOLATILE: case QCC_KW_RESTRICT:
    case QCC_KW_ATOMIC:  case QCC_KW_INLINE:   case QCC_KW_NORETURN:
    case QCC_KW_VOID:    case QCC_KW_CHAR:     case QCC_KW_SHORT:
    case QCC_KW_INT:     case QCC_KW_LONG:     case QCC_KW_FLOAT:
    case QCC_KW_DOUBLE:  case QCC_KW_SIGNED:   case QCC_KW_UNSIGNED:
    case QCC_KW_BOOL:    case QCC_KW_COMPLEX:  case QCC_KW_IMAGINARY:
    case QCC_KW_STRUCT:  case QCC_KW_UNION:    case QCC_KW_ENUM:
        return 1;
    default:
        return 0;
    }
}

static qcc_storage_class map_storage(qcc_keyword kw)
{
    switch (kw) {
    case QCC_KW_TYPEDEF:      return QCC_SC_TYPEDEF;
    case QCC_KW_EXTERN:       return QCC_SC_EXTERN;
    case QCC_KW_STATIC:       return QCC_SC_STATIC;
    case QCC_KW_THREAD_LOCAL: return QCC_SC_THREAD_LOCAL;
    case QCC_KW_AUTO:         return QCC_SC_AUTO;
    case QCC_KW_REGISTER:     return QCC_SC_REGISTER;
    default:                  return QCC_SC_NONE;
    }
}

/* Consume a run of type-qualifier keywords (§6.7.3), returning their bitmask. */
static unsigned parse_type_qualifier_list(qcc_parser *p)
{
    unsigned q = 0;
    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind != QCC_TOKEN_KEYWORD) {
            break;
        }
        if (t->keyword == QCC_KW_CONST) {
            q |= QCC_QUAL_CONST;
        } else if (t->keyword == QCC_KW_VOLATILE) {
            q |= QCC_QUAL_VOLATILE;
        } else if (t->keyword == QCC_KW_RESTRICT) {
            q |= QCC_QUAL_RESTRICT;
        } else if (t->keyword == QCC_KW_ATOMIC) {
            q |= QCC_QUAL_ATOMIC;
        } else {
            break;
        }
        advance(p);
    }
    return q;
}

/* Skip a balanced { } block (cursor at '{'); used to recover past an unsupported
   struct/union/enum definition. */
static void skip_balanced_braces(qcc_parser *p)
{
    if (!at_punct(p, QCC_PUNCT_LBRACE)) {
        return;
    }
    int depth = 0;
    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind == QCC_TOKEN_EOF) {
            return;
        }
        if (t->kind == QCC_TOKEN_PUNCT && t->punct == QCC_PUNCT_LBRACE) {
            ++depth;
        } else if (t->kind == QCC_TOKEN_PUNCT && t->punct == QCC_PUNCT_RBRACE) {
            --depth;
            advance(p);
            if (depth == 0) {
                return;
            }
            continue;
        }
        advance(p);
    }
}

/* Map the arithmetic type-specifier multiset to a basic type (§6.7.2 ¶2). */
static const qcc_type *base_from_counts(qcc_parser *p, const declspec *ds,
                                        const qcc_token *loc)
{
    int           u = ds->c_unsigned > 0;
    int           s = ds->c_signed > 0;
    qcc_type_kind k;

    if (ds->c_void) {
        k = QCC_TYPE_VOID;
    } else if (ds->c_bool) {
        k = QCC_TYPE_BOOL;
    } else if (ds->c_double) {
        k = (ds->c_long >= 1) ? QCC_TYPE_LDOUBLE : QCC_TYPE_DOUBLE;
    } else if (ds->c_float) {
        k = QCC_TYPE_FLOAT;
    } else if (ds->c_char) {
        k = s ? QCC_TYPE_SCHAR : (u ? QCC_TYPE_UCHAR : QCC_TYPE_CHAR);
    } else if (ds->c_short) {
        k = u ? QCC_TYPE_USHORT : QCC_TYPE_SHORT;
    } else if (ds->c_long >= 2) {
        k = u ? QCC_TYPE_ULLONG : QCC_TYPE_LLONG;
    } else if (ds->c_long == 1) {
        k = u ? QCC_TYPE_ULONG : QCC_TYPE_LONG;
    } else {
        k = u ? QCC_TYPE_UINT : QCC_TYPE_INT; /* int / signed / unsigned [int] */
    }

    if ((u && s) || ds->c_int > 1 || ds->c_long > 2 || ds->c_char > 1 ||
        ds->c_short > 1) {
        parse_error(p, loc, "invalid combination of type specifiers");
    }

    const qcc_type *t = qcc_type_basic(p->types, k);
    if (t == NULL) {
        p->oom = 1;
    }
    return t;
}

/* The base type a declaration-specifier list denotes, qualifiers applied. */
static const qcc_type *ds_base_type(qcc_parser *p, const declspec *ds,
                                    const qcc_token *loc)
{
    const qcc_type *base;
    if (ds->has_named_type) {
        base = ds->named_type;
    } else if (ds->type_specifier_count == 0) {
        parse_error(p, loc, "expected a type specifier");
        base = qcc_type_basic(p->types, QCC_TYPE_INT); /* recover as int */
    } else {
        base = base_from_counts(p, ds, loc);
    }
    if (base == NULL) {
        if (!p->oom) {
            p->oom = 1; /* qcc_type_basic failed. */
        }
        return NULL;
    }
    if (ds->quals != 0) {
        const qcc_type *q = qcc_type_qualified(p->types, base, ds->quals);
        if (q == NULL) {
            p->oom = 1;
            return NULL;
        }
        base = q;
    }
    return base;
}

/* §6.7.2.2 ¶2: an enumerator's value must be representable as an int. */
static int enum_value_fits_int(int64_t v)
{
    return v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX;
}

/* Recover from a malformed enumerator body by consuming up to and including the
   closing '}'. The body holds only constant expressions (no nested braces), so the
   first '}' ends it; stops at EOF so a missing '}' cannot loop forever. */
static void skip_to_rbrace(qcc_parser *p)
{
    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind == QCC_TOKEN_EOF) {
            return;
        }
        if (t->kind == QCC_TOKEN_PUNCT && t->punct == QCC_PUNCT_RBRACE) {
            advance(p);
            return;
        }
        advance(p);
    }
}

/*
 * struct-or-union-specifier (§6.7.2.1), the `struct`/`union` keyword already
 * consumed. A definition needs member layout per the System V x86-64 psABI and is
 * deferred (ADR-0026): a braced body is diagnosed and skipped, leaving an
 * incomplete tagged type so the surrounding declaration still parses. Returns the
 * tagged type, or NULL only on out-of-memory (p->oom set).
 */
static const qcc_type *parse_struct_union_specifier(qcc_parser *p,
                                                    qcc_type_kind kind,
                                                    const qcc_token *kw)
{
    (void)kw;
    const qcc_token *tag = NULL;
    if (peek(p)->kind == QCC_TOKEN_IDENTIFIER) {
        tag = peek(p);
        advance(p);
    }
    if (at_punct(p, QCC_PUNCT_LBRACE)) {
        parse_error(p, peek(p), "struct/union definitions are not supported yet");
        skip_balanced_braces(p);
    }
    const qcc_type *tt = qcc_type_tagged(p->types, kind,
                                         tag ? tag->spelling : NULL,
                                         tag ? tag->spelling_len : 0, 0);
    if (tt == NULL) {
        p->oom = 1;
    }
    return tt;
}

/*
 * enum-specifier (§6.7.2.2), the `enum` keyword already consumed (`kw` locates it
 * for diagnostics). Three shapes:
 *   - `enum tag`              — a reference (§6.7.2.3): resolve `tag` to its visible
 *                               (possibly complete) type, else an incomplete enum.
 *   - `enum [tag] { list }`   — a definition: parse the enumerator-list, give each
 *                               constant a value (an explicit `= constant-expression`
 *                               via consteval, else the previous value + 1, the first
 *                               0, §6.7.2.2 ¶3), register it (type int, §6.7.2.2 ¶3),
 *                               complete the type and register the tag (§6.7.2.3).
 * Returns the enum type, or NULL only on out-of-memory (p->oom set); recoverable
 * syntax errors set p->had_error but still yield a type so the declaration parses.
 */
static const qcc_type *parse_enum_specifier(qcc_parser *p, const qcc_token *kw)
{
    const qcc_token *tag = NULL;
    if (peek(p)->kind == QCC_TOKEN_IDENTIFIER) {
        tag = peek(p);
        advance(p);
    }

    if (!at_punct(p, QCC_PUNCT_LBRACE)) {
        /* A reference. Use the visible definition if there is one (§6.7.2.3 ¶7
           requires the type be complete; we are lenient and forward-declare). */
        if (tag != NULL && p->syms != NULL) {
            const qcc_symbol *prev = qcc_symtab_lookup(
                p->syms, tag->spelling, tag->spelling_len, QCC_NS_TAG);
            if (prev != NULL && prev->type != NULL &&
                prev->type->kind == QCC_TYPE_ENUM) {
                return prev->type;
            }
        }
        const qcc_type *ref = qcc_type_tagged(
            p->types, QCC_TYPE_ENUM, tag ? tag->spelling : NULL,
            tag ? tag->spelling_len : 0, 0);
        if (ref == NULL) {
            p->oom = 1;
        }
        return ref;
    }

    advance(p); /* '{' — a definition begins. */

    /* §6.7.2.3 ¶8: a tag may be defined only once in a scope. Skipped inside a
       parameter-type-list, where nothing is registered (see below). */
    if (tag != NULL && p->syms != NULL && !p->in_param_list) {
        const qcc_symbol *prev = qcc_symtab_lookup_current_scope(
            p->syms, tag->spelling, tag->spelling_len, QCC_NS_TAG);
        if (prev != NULL && prev->type != NULL && prev->type->complete) {
            parse_error(p, tag, "redefinition of enumeration tag");
        }
    }

    const qcc_type *int_ty = qcc_type_basic(p->types, QCC_TYPE_INT);
    if (int_ty == NULL) {
        p->oom = 1;
        return NULL;
    }

    if (at_punct(p, QCC_PUNCT_RBRACE)) {
        /* §6.7.2.2 ¶1: the enumerator-list shall have at least one enumerator. */
        parse_error(p, kw, "an enumeration requires at least one enumerator");
        advance(p); /* consume the '}' */
    } else {
        int64_t next  = 0; /* §6.7.2.2 ¶3: the first unspecified enumerator is 0. */
        int     broke = 0; /* set once recovery has consumed past the '}' */
        for (;;) {
            if (peek(p)->kind != QCC_TOKEN_IDENTIFIER) {
                parse_error(p, peek(p), "expected an enumeration constant");
                skip_to_rbrace(p);
                broke = 1;
                break;
            }
            const qcc_token *name  = peek(p);
            int64_t          value = next;
            advance(p);

            if (at_punct(p, QCC_PUNCT_EQ)) {
                advance(p);
                qcc_expr *ve = parse_conditional(p); /* constant-expression §6.6 */
                if (ve == NULL) {
                    if (!p->oom) {
                        skip_to_rbrace(p);
                        broke = 1;
                    }
                    break;
                }
                qcc_const_value cv;
                if (qcc_eval_const_int(ve, &cv) != QCC_OK) {
                    parse_error(p, name, "enumerator value is not an integer "
                                         "constant expression");
                } else {
                    value = (int64_t)cv.value;
                }
            }
            if (!enum_value_fits_int(value)) {
                parse_error(p, name,
                            "enumerator value is not representable as int");
            }

            /* A constant defined in a parameter-type-list has prototype scope
               (§6.2.1 ¶4) we do not model, so register only outside one. */
            if (p->syms != NULL && !p->in_param_list) {
                if (qcc_symtab_lookup_current_scope(p->syms, name->spelling,
                                                    name->spelling_len,
                                                    QCC_NS_ORDINARY) != NULL) {
                    parse_error(p, name, "redeclaration of enumeration constant");
                }
                if (qcc_symtab_insert_enum_const(
                        p->syms, name->spelling, name->spelling_len, int_ty, value,
                        name->source, name->offset, name->line, name->column,
                        NULL) != QCC_OK) {
                    p->oom = 1;
                    return NULL;
                }
            }
            next = value + 1; /* §6.7.2.2 ¶3: the next defaults to one more. */

            if (at_punct(p, QCC_PUNCT_COMMA)) {
                advance(p);
                if (at_punct(p, QCC_PUNCT_RBRACE)) {
                    break; /* §6.7.2.2: a trailing comma is permitted. */
                }
                continue;
            }
            break;
        }

        if (!broke && !expect_punct(p, QCC_PUNCT_RBRACE,
                                    "expected ',' or '}' in enumerator list")) {
            skip_to_rbrace(p);
        }
        if (p->oom) {
            return NULL;
        }
    }

    const qcc_type *enum_ty = qcc_type_tagged(
        p->types, QCC_TYPE_ENUM, tag ? tag->spelling : NULL,
        tag ? tag->spelling_len : 0, 1 /* complete */);
    if (enum_ty == NULL) {
        p->oom = 1;
        return NULL;
    }
    if (tag != NULL && p->syms != NULL && !p->in_param_list &&
        qcc_symtab_insert(p->syms, tag->spelling, tag->spelling_len, QCC_NS_TAG,
                          QCC_SYM_TAG, enum_ty, tag->source, tag->offset, tag->line,
                          tag->column, NULL) != QCC_OK) {
        p->oom = 1;
        return NULL;
    }
    return enum_ty;
}

static void parse_declaration_specifiers(qcc_parser *p, declspec *ds)
{
    memset(ds, 0, sizeof(*ds));
    for (;;) {
        const qcc_token *t = peek(p);
        if (t->kind == QCC_TOKEN_KEYWORD) {
            qcc_keyword kw = t->keyword;
            switch (kw) {
            case QCC_KW_TYPEDEF: case QCC_KW_EXTERN: case QCC_KW_STATIC:
            case QCC_KW_AUTO: case QCC_KW_REGISTER: case QCC_KW_THREAD_LOCAL: {
                qcc_storage_class sc = map_storage(kw);
                if (ds->storage_set && ds->storage != sc &&
                    ds->storage != QCC_SC_THREAD_LOCAL &&
                    sc != QCC_SC_THREAD_LOCAL) {
                    parse_error(p, t, "multiple storage-class specifiers");
                }
                if (sc != QCC_SC_THREAD_LOCAL || !ds->storage_set) {
                    ds->storage = sc;
                }
                ds->storage_set = 1;
                advance(p);
                continue;
            }
            case QCC_KW_CONST:    ds->quals |= QCC_QUAL_CONST;    advance(p); continue;
            case QCC_KW_VOLATILE: ds->quals |= QCC_QUAL_VOLATILE; advance(p); continue;
            case QCC_KW_RESTRICT: ds->quals |= QCC_QUAL_RESTRICT; advance(p); continue;
            case QCC_KW_ATOMIC:   ds->quals |= QCC_QUAL_ATOMIC;   advance(p); continue;
            case QCC_KW_INLINE:   ds->func_spec |= QCC_FS_INLINE;   advance(p); continue;
            case QCC_KW_NORETURN: ds->func_spec |= QCC_FS_NORETURN; advance(p); continue;
            case QCC_KW_VOID:  ds->c_void++;  ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_BOOL:  ds->c_bool++;  ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_CHAR:  ds->c_char++;  ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_SHORT: ds->c_short++; ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_INT:   ds->c_int++;   ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_LONG:  ds->c_long++;  ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_FLOAT: ds->c_float++; ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_DOUBLE: ds->c_double++; ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_SIGNED:   ds->c_signed++;   ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_UNSIGNED: ds->c_unsigned++; ds->type_specifier_count++; advance(p); continue;
            case QCC_KW_COMPLEX: case QCC_KW_IMAGINARY:
                parse_error(p, t, "_Complex/_Imaginary are not supported yet");
                advance(p);
                continue;
            case QCC_KW_STRUCT: case QCC_KW_UNION: case QCC_KW_ENUM: {
                advance(p); /* struct / union / enum keyword */
                const qcc_type *tt =
                    (kw == QCC_KW_ENUM)
                        ? parse_enum_specifier(p, t)
                        : parse_struct_union_specifier(
                              p,
                              (kw == QCC_KW_STRUCT) ? QCC_TYPE_STRUCT
                                                    : QCC_TYPE_UNION,
                              t);
                if (tt == NULL) {
                    return; /* out-of-memory; p->oom set by the helper. */
                }
                ds->named_type     = tt;
                ds->has_named_type = 1;
                ds->type_specifier_count++;
                continue;
            }
            default:
                return; /* Not a specifier keyword: the declarator begins. */
            }
        } else if (t->kind == QCC_TOKEN_IDENTIFIER) {
            /* A typedef-name is a type specifier only if no type is set yet
               (otherwise it is the declared name). */
            if (!ds->has_named_type && ds->type_specifier_count == 0 &&
                p->syms != NULL &&
                qcc_symtab_is_typedef_name(p->syms, t->spelling,
                                           t->spelling_len)) {
                const qcc_symbol *sym = qcc_symtab_lookup(
                    p->syms, t->spelling, t->spelling_len, QCC_NS_ORDINARY);
                ds->named_type     = sym->type;
                ds->has_named_type = 1;
                ds->type_specifier_count++;
                advance(p);
                continue;
            }
            return;
        } else {
            return;
        }
    }
}

/* §6.7.6.3: a parameter of array/function type is adjusted to a pointer. */
static const qcc_type *adjust_param_type(qcc_parser *p, const qcc_type *t)
{
    if (t->kind == QCC_TYPE_ARRAY) {
        const qcc_type *pt = qcc_type_pointer(p->types, t->element, 0);
        if (pt == NULL) {
            p->oom = 1;
        }
        return pt;
    }
    if (t->kind == QCC_TYPE_FUNCTION) {
        const qcc_type *pt = qcc_type_pointer(p->types, t, 0);
        if (pt == NULL) {
            p->oom = 1;
        }
        return pt;
    }
    return t;
}

/* A function declarator's parameter list (§6.7.6.3); '(' already consumed. */
static const qcc_type *parse_func_params(qcc_parser *p, const qcc_type *ret)
{
    const qcc_type **params   = NULL;
    qcc_param       *caps     = NULL; /* Parallel (name,type); only when capturing. */
    size_t           n        = 0;
    size_t           cap      = 0;
    int              variadic = 0;
    int              capture  = p->capture_params;

    int void_only = (at_keyword(p, QCC_KW_VOID) && p->pos + 1 < p->count &&
                     p->tokens[p->pos + 1].kind == QCC_TOKEN_PUNCT &&
                     p->tokens[p->pos + 1].punct == QCC_PUNCT_RPAREN);

    if (at_punct(p, QCC_PUNCT_RPAREN)) {
        advance(p); /* () — no parameter information. */
    } else if (void_only) {
        advance(p); /* void */
        advance(p); /* )    */
    } else {
        for (;;) {
            if (at_punct(p, QCC_PUNCT_ELLIPSIS)) {
                advance(p);
                variadic = 1;
                break;
            }
            const qcc_token *ploc = peek(p);
            declspec         ds;
            int              saved_in_param = p->in_param_list;
            p->in_param_list = 1; /* suppress prototype-scope tag registration */
            parse_declaration_specifiers(p, &ds);
            p->in_param_list = saved_in_param;
            const qcc_type  *pbase = p->oom ? NULL : ds_base_type(p, &ds, ploc);
            const qcc_token *pname = NULL;
            const qcc_type  *pt =
                (pbase == NULL) ? NULL : parse_declarator(p, pbase, 1, &pname);
            if (pt != NULL) {
                pt = adjust_param_type(p, pt);
            }
            if (pt == NULL) {
                free(params);
                free(caps);
                return NULL; /* had_error / oom set by the failing step. */
            }
            if (n == cap) {
                size_t           ncap  = (cap == 0) ? 4u : cap * 2u;
                const qcc_type **grown = (const qcc_type **)realloc(
                    params, ncap * sizeof(*grown));
                if (grown == NULL) {
                    free(params);
                    free(caps);
                    p->oom = 1;
                    return NULL;
                }
                params = grown;
                if (capture) {
                    qcc_param *gc =
                        (qcc_param *)realloc(caps, ncap * sizeof(*gc));
                    if (gc == NULL) {
                        free(params);
                        free(caps);
                        p->oom = 1;
                        return NULL;
                    }
                    caps = gc;
                }
                cap = ncap;
            }
            params[n] = pt;
            if (capture) {
                caps[n].name     = (pname != NULL) ? pname->spelling : NULL;
                caps[n].name_len = (pname != NULL) ? pname->spelling_len : 0;
                caps[n].type     = pt;
            }
            ++n;
            if (at_punct(p, QCC_PUNCT_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after parameters")) {
            free(params);
            free(caps);
            return NULL;
        }
    }

    const qcc_type *ft = qcc_type_function(p->types, ret, params, n, variadic);
    free(params);
    if (ft == NULL) {
        free(caps);
        p->oom = 1;
        return NULL;
    }
    if (capture) {
        /* The innermost function declarator's parameter list completes last, so
           this stores the defined function's own parameters (ADR-0024). */
        p->cap_params      = NULL;
        p->cap_param_count = 0;
        if (n != 0) {
            qcc_param *owned =
                (qcc_param *)qcc_ast_dup(p->ast, caps, n * sizeof(*caps));
            if (owned == NULL) {
                free(caps);
                p->oom = 1;
                return NULL;
            }
            p->cap_params      = owned;
            p->cap_param_count = n;
        }
    }
    free(caps);
    return ft;
}

/* Array/function suffixes of a (direct) declarator. Recurses so the leftmost
   suffix binds outermost (§6.7.6: `a[2][3]` is array[2] of array[3]). */
static const qcc_type *parse_type_suffix(qcc_parser *p, const qcc_type *type,
                                         int abstract)
{
    if (at_punct(p, QCC_PUNCT_LPAREN)) {
        advance(p);
        return parse_func_params(p, type);
    }
    if (at_punct(p, QCC_PUNCT_LBRACKET)) {
        advance(p);
        uint64_t len      = 0;
        int      complete = 0;
        if (!at_punct(p, QCC_PUNCT_RBRACKET)) {
            qcc_expr *sz = parse_assignment(p);
            if (sz == NULL) {
                return NULL;
            }
            /* §6.7.6.2: a non-VLA bound is an integer constant expression (§6.6).
               Evaluate it; a non-constant bound stays incomplete (VLAs are not
               supported yet). */
            qcc_const_value cv;
            if (qcc_eval_const_int(sz, &cv) == QCC_OK) {
                len      = cv.value;
                complete = 1;
            }
        }
        if (!expect_punct(p, QCC_PUNCT_RBRACKET,
                          "expected ']' in array declarator")) {
            return NULL;
        }
        const qcc_type *inner = parse_type_suffix(p, type, abstract);
        if (inner == NULL) {
            return NULL;
        }
        const qcc_type *arr = qcc_type_array(p->types, inner, len, complete);
        if (arr == NULL) {
            p->oom = 1;
        }
        return arr;
    }
    return type;
}

/* direct-declarator (§6.7.6): an identifier (or, when abstract, none), or a
   parenthesised declarator, followed by array/function suffixes. */
static const qcc_type *parse_direct_declarator(qcc_parser *p, const qcc_type *type,
                                               int abstract,
                                               const qcc_token **out_name)
{
    int nested = 0;
    if (at_punct(p, QCC_PUNCT_LPAREN)) {
        if (!abstract) {
            nested = 1; /* Concrete: '(' here always groups a declarator. */
        } else if (p->pos + 1 < p->count) {
            /* Abstract: '(' groups only if a declarator (not a param list)
               follows; a param list begins with a specifier or ')'. */
            const qcc_token *nx = &p->tokens[p->pos + 1];
            nested = (nx->kind == QCC_TOKEN_PUNCT &&
                      (nx->punct == QCC_PUNCT_STAR ||
                       nx->punct == QCC_PUNCT_LPAREN ||
                       nx->punct == QCC_PUNCT_LBRACKET));
        }
    }

    if (nested) {
        size_t start = p->pos; /* at '(' */
        advance(p);
        const qcc_token *dummy = NULL;
        if (parse_declarator(p, type, abstract, &dummy) == NULL) {
            return NULL; /* skip pass; error already recorded */
        }
        if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' in declarator")) {
            return NULL;
        }
        const qcc_type *suffixed = parse_type_suffix(p, type, abstract);
        if (suffixed == NULL) {
            return NULL;
        }
        size_t end = p->pos;
        p->pos     = start + 1; /* re-parse the inner declarator with the real type */
        const qcc_type *result = parse_declarator(p, suffixed, abstract, out_name);
        if (result == NULL) {
            return NULL;
        }
        p->pos = end;
        return result;
    }

    if (peek(p)->kind == QCC_TOKEN_IDENTIFIER) {
        if (out_name != NULL) {
            *out_name = peek(p);
        }
        advance(p);
    } else if (!abstract) {
        parse_error(p, peek(p), "expected an identifier in declarator");
        return NULL;
    }
    return parse_type_suffix(p, type, abstract);
}

/* declarator (§6.7.6): optional pointers then a direct-declarator. */
static const qcc_type *parse_declarator(qcc_parser *p, const qcc_type *base,
                                        int abstract, const qcc_token **out_name)
{
    const qcc_type *type = base;
    while (at_punct(p, QCC_PUNCT_STAR)) {
        advance(p);
        unsigned q = parse_type_qualifier_list(p);
        type = qcc_type_pointer(p->types, type, q);
        if (type == NULL) {
            p->oom = 1;
            return NULL;
        }
    }
    return parse_direct_declarator(p, type, abstract, out_name);
}

/* A type-name (§6.7.7): a specifier-qualifier list followed by an optional
   abstract declarator (a declarator with no identifier). Returns the type, or
   NULL with p->oom or p->had_error set. The single code path the public
   qcc_parse_type_name and the cast / sizeof / _Alignof hooks share. */
static const qcc_type *parse_type_name(qcc_parser *p)
{
    if (p->types == NULL) {
        /* A type-name cannot be built without a type context (expression-only
           use, §6.7.7 needs the type module); diagnose instead of crashing. */
        parse_error(p, peek(p), "a type name is not valid here");
        return NULL;
    }
    const qcc_token *loc = peek(p);
    declspec         ds;
    parse_declaration_specifiers(p, &ds);
    if (p->oom) {
        return NULL;
    }
    const qcc_type *base = ds_base_type(p, &ds, loc);
    if (base == NULL) {
        return NULL;
    }
    const qcc_token *name = NULL;
    const qcc_type  *t    = parse_declarator(p, base, 1, &name);
    if (t == NULL) {
        return NULL;
    }
    if (name != NULL) {
        /* §6.7.7: a type-name is an abstract declarator — it declares no name. */
        parse_error(p, name, "a type name cannot declare an identifier");
    }
    return t;
}

qcc_status qcc_parse_type_name(qcc_parser *p, const qcc_type **out)
{
    if (p == NULL || out == NULL || p->types == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;

    const qcc_type *t = parse_type_name(p);
    if (t == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }
    *out = t;
    return p->had_error ? QCC_ERR_PARSE : QCC_OK;
}

int qcc_parser_at_declaration(const qcc_parser *p)
{
    const qcc_token *t = peek(p);
    if (t->kind == QCC_TOKEN_KEYWORD) {
        /* _Static_assert is a (static_assert-)declaration, §6.7.10. */
        return is_decl_specifier_keyword(t->keyword) ||
               t->keyword == QCC_KW_STATIC_ASSERT;
    }
    if (t->kind == QCC_TOKEN_IDENTIFIER) {
        return p->syms != NULL &&
               qcc_symtab_is_typedef_name(p->syms, t->spelling, t->spelling_len);
    }
    return 0;
}

/*
 * static_assert-declaration (§6.7.10): `_Static_assert ( constant-expression ,
 * string-literal ) ;`. The controlling expression is an integer constant
 * expression (§6.6), evaluated now via the constant evaluator (ADR-0025); a zero
 * value fails the assertion and is diagnosed with the string-literal message. It
 * declares no entity, so it contributes nothing to the declarator list.
 */
static qcc_status parse_static_assert(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* _Static_assert */
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after _Static_assert")) {
        return QCC_ERR_PARSE;
    }
    qcc_expr *cond = parse_conditional(p);
    if (cond == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }
    if (!expect_punct(p, QCC_PUNCT_COMMA, "expected ',' in _Static_assert")) {
        return QCC_ERR_PARSE;
    }
    const qcc_token *msg = peek(p);
    if (msg->kind != QCC_TOKEN_STRING) {
        parse_error(p, msg, "expected a string literal in _Static_assert");
        return QCC_ERR_PARSE;
    }
    advance(p);
    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after _Static_assert") ||
        !expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after _Static_assert")) {
        return QCC_ERR_PARSE;
    }

    qcc_const_value cv;
    if (qcc_eval_const_int(cond, &cv) != QCC_OK) {
        parse_error(p, kw, "_Static_assert expression is not an integer "
                           "constant expression");
        return QCC_ERR_PARSE;
    }
    if (cv.value == 0) {
        qcc_status st;
        if ((msg->char_encoding == QCC_ENC_PLAIN ||
             msg->char_encoding == QCC_ENC_UTF8) &&
            msg->str_data != NULL) {
            st = qcc_diag_emit(p->diags, QCC_DIAG_ERROR, kw->source, kw->offset,
                               tok_span(kw), "static assertion failed: %.*s",
                               (int)msg->str_len, (const char *)msg->str_data);
        } else {
            st = qcc_diag_emit(p->diags, QCC_DIAG_ERROR, kw->source, kw->offset,
                               tok_span(kw), "static assertion failed");
        }
        if (st != QCC_OK) {
            p->oom = 1;
        }
        p->had_error = 1;
        return QCC_ERR_PARSE;
    }
    return p->had_error ? QCC_ERR_PARSE : QCC_OK;
}

/*
 * Parse the init-declarator list of a declaration given already-parsed specifiers
 * (`ds`/`base`, with `specloc` for the provenance of an abstract declarator). The
 * cursor is positioned just past the declaration-specifiers. Each declared name is
 * registered (§6.7.8 typedef-names go in as such) and appended to `out` as a
 * qcc_decl; the no-declarator case (a bare ';' after, e.g., a struct/enum
 * specifier) and the trailing ';' are handled here.
 *
 * Splitting this out of qcc_parse_declaration lets qcc_parse_external_declaration
 * parse the specifiers exactly once: a tagged-type specifier — an enum definition
 * (ADR-0026) — registers its tag and constants as a side effect, which must not run
 * twice, so the external-declaration path can no longer re-parse from the start.
 */
static qcc_status parse_init_declarators(qcc_parser *p, const declspec *ds,
                                         const qcc_type *base,
                                         const qcc_token *specloc,
                                         qcc_decl_list *out)
{
    /* A declaration with no declarators (e.g. `struct foo;`, `enum E { A };`). */
    if (at_punct(p, QCC_PUNCT_SEMI)) {
        advance(p);
        return p->had_error ? QCC_ERR_PARSE : QCC_OK;
    }

    for (;;) {
        const qcc_token *name = NULL;
        const qcc_type  *full = parse_declarator(p, base, 0, &name);
        if (full == NULL) {
            return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
        }

        qcc_expr *init = NULL;
        if (at_punct(p, QCC_PUNCT_EQ)) {
            advance(p);
            init = parse_assignment(p); /* Scalar initializer; braces deferred. */
            if (init == NULL) {
                return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
            }
        }

        qcc_decl d;
        memset(&d, 0, sizeof(d));
        d.storage   = ds->storage;
        d.func_spec = ds->func_spec;
        d.type      = full;
        d.init      = init;
        if (name != NULL) {
            d.name     = name->spelling;
            d.name_len = name->spelling_len;
            d.source   = name->source;
            d.offset   = name->offset;
            d.line     = name->line;
            d.column   = name->column;

            qcc_sym_kind k = (ds->storage == QCC_SC_TYPEDEF)
                                 ? QCC_SYM_TYPEDEF
                                 : (full->kind == QCC_TYPE_FUNCTION
                                        ? QCC_SYM_FUNCTION
                                        : QCC_SYM_OBJECT);
            if (qcc_symtab_insert(p->syms, d.name, d.name_len, QCC_NS_ORDINARY, k,
                                  full, d.source, d.offset, d.line, d.column,
                                  NULL) != QCC_OK) {
                return QCC_ERR_OUT_OF_MEMORY;
            }
        } else {
            d.source = specloc->source;
            d.offset = specloc->offset;
            d.line   = specloc->line;
            d.column = specloc->column;
        }

        if (qcc_decl_list_push(out, &d) != QCC_OK) {
            return QCC_ERR_OUT_OF_MEMORY;
        }

        if (at_punct(p, QCC_PUNCT_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }

    if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after declaration")) {
        return QCC_ERR_PARSE;
    }
    return p->had_error ? QCC_ERR_PARSE : QCC_OK;
}

qcc_status qcc_parse_declaration(qcc_parser *p, qcc_decl_list *out)
{
    if (p == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (p->types == NULL || p->syms == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    if (at_keyword(p, QCC_KW_STATIC_ASSERT)) {
        return parse_static_assert(p);
    }

    const qcc_token *specloc = peek(p);
    declspec         ds;
    parse_declaration_specifiers(p, &ds);
    if (p->oom) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    const qcc_type *base = ds_base_type(p, &ds, specloc);
    if (base == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }
    return parse_init_declarators(p, &ds, base, specloc, out);
}

/*
 * Statements (§6.8; ADR-0023 Unit 3). Recursive descent dispatching on the leading
 * token. A compound statement, and a `for` with a declaration in its first clause,
 * open a block scope (§6.2.1) around their contents so declaration parsing and the
 * §6.7.8 typedef test resolve at the right depth. A parse stops at the first syntax
 * error (no panic-mode recovery yet); semantic checks (a `break` outside a loop, an
 * undefined `goto` target, a `case` outside a switch, …) are a later pass.
 */

static qcc_stmt *parse_statement(qcc_parser *p);

/* Mark a statement-node allocation failure and return NULL. */
static qcc_stmt *fail_oom_stmt(qcc_parser *p)
{
    p->oom = 1;
    return NULL;
}

/* Does the cursor look at `identifier :` — a labeled statement (§6.8.1)? Checked
   before the declaration test because a label may even spell a typedef-name
   (labels are a separate name space, §6.2.3). */
static int at_label(const qcc_parser *p)
{
    if (peek(p)->kind != QCC_TOKEN_IDENTIFIER || p->pos + 1 >= p->count) {
        return 0;
    }
    const qcc_token *nx = &p->tokens[p->pos + 1];
    return nx->kind == QCC_TOKEN_PUNCT && nx->punct == QCC_PUNCT_COLON;
}

/* A declaration as a block item (§6.8.2): parse it (consuming its ';') and wrap
   the resulting init-declarators in a DECL statement copied into the ast. */
static qcc_stmt *parse_decl_stmt(qcc_parser *p)
{
    const qcc_token *loc = peek(p);
    qcc_decl_list    decls;
    qcc_decl_list_init(&decls);
    qcc_status st = qcc_parse_declaration(p, &decls);
    if (st != QCC_OK) {
        qcc_decl_list_dispose(&decls);
        return NULL; /* had_error / oom already set by qcc_parse_declaration. */
    }
    qcc_stmt *s = qcc_stmt_decl(p->ast, decls.items, decls.count, loc);
    qcc_decl_list_dispose(&decls);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* expression-statement, including the null statement `;` (§6.8.3). */
static qcc_stmt *parse_expression_statement(qcc_parser *p)
{
    const qcc_token *loc = peek(p);
    if (at_punct(p, QCC_PUNCT_SEMI)) {
        advance(p);
        qcc_stmt *s = qcc_stmt_expr(p->ast, NULL, loc); /* the null statement */
        return (s != NULL) ? s : fail_oom_stmt(p);
    }
    qcc_expr *e = parse_expression(p);
    if (e == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after expression")) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_expr(p->ast, e, loc);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* The block-item list of a compound statement (§6.8.2); '{' at the cursor. The
   surrounding block scope is opened/closed by parse_compound_statement. */
static qcc_stmt *parse_block_items(qcc_parser *p, const qcc_token *lbrace)
{
    advance(p); /* '{' */
    qcc_stmt **items = NULL;
    size_t     n     = 0;
    size_t     cap   = 0;

    while (!at_punct(p, QCC_PUNCT_RBRACE) && peek(p)->kind != QCC_TOKEN_EOF) {
        qcc_stmt *item = (!at_label(p) && qcc_parser_at_declaration(p))
                             ? parse_decl_stmt(p)
                             : parse_statement(p);
        if (item == NULL) {
            free(items);
            return NULL;
        }
        if (n == cap) {
            size_t     ncap  = (cap == 0) ? 8u : cap * 2u;
            qcc_stmt **grown = (qcc_stmt **)realloc(items, ncap * sizeof(*grown));
            if (grown == NULL) {
                free(items);
                return fail_oom_stmt(p);
            }
            items = grown;
            cap   = ncap;
        }
        items[n++] = item;
    }
    if (!expect_punct(p, QCC_PUNCT_RBRACE, "expected '}' to close block")) {
        free(items);
        return NULL;
    }
    qcc_stmt *c = qcc_stmt_compound(p->ast, items, n, lbrace);
    free(items);
    return (c != NULL) ? c : fail_oom_stmt(p);
}

/* compound-statement (§6.8.2): a brace-delimited block; opens a block scope. */
static qcc_stmt *parse_compound_statement(qcc_parser *p)
{
    const qcc_token *lbrace = peek(p); /* '{' */
    if (qcc_symtab_push_scope(p->syms, QCC_SCOPE_BLOCK) != QCC_OK) {
        return fail_oom_stmt(p);
    }
    qcc_stmt *s = parse_block_items(p, lbrace);
    qcc_symtab_pop_scope(p->syms);
    return s;
}

/* if-statement (§6.8.4.1). The else binds to this if: the then-branch is parsed
   first and greedily consumes any else, which is the nearest-if rule. */
static qcc_stmt *parse_if_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* if */
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after 'if'")) {
        return NULL;
    }
    qcc_expr *cond = parse_expression(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after if condition")) {
        return NULL;
    }
    qcc_stmt *then_s = parse_statement(p);
    if (then_s == NULL) {
        return NULL;
    }
    qcc_stmt *else_s = NULL;
    if (at_keyword(p, QCC_KW_ELSE)) {
        advance(p);
        else_s = parse_statement(p);
        if (else_s == NULL) {
            return NULL;
        }
    }
    qcc_stmt *s = qcc_stmt_if(p->ast, cond, then_s, else_s, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* switch-statement (§6.8.4.2). */
static qcc_stmt *parse_switch_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* switch */
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after 'switch'")) {
        return NULL;
    }
    qcc_expr *cond = parse_expression(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after switch expression")) {
        return NULL;
    }
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_switch(p->ast, cond, body, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* while-statement (§6.8.5.1). */
static qcc_stmt *parse_while_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* while */
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after 'while'")) {
        return NULL;
    }
    qcc_expr *cond = parse_expression(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after while condition")) {
        return NULL;
    }
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_while(p->ast, cond, body, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* do-while-statement (§6.8.5.2). */
static qcc_stmt *parse_do_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* do */
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    if (!at_keyword(p, QCC_KW_WHILE)) {
        parse_error(p, peek(p), "expected 'while' after do-statement body");
        return NULL;
    }
    advance(p); /* while */
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after 'while'")) {
        return NULL;
    }
    qcc_expr *cond = parse_expression(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_RPAREN,
                      "expected ')' after do-while condition") ||
        !expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after do-while statement")) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_do(p->ast, body, cond, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* The clauses and body of a for-statement (§6.8.5.3); the block scope that holds
   a clause-1 declaration is opened/closed by parse_for_statement. */
static qcc_stmt *parse_for_inner(qcc_parser *p, const qcc_token *kw)
{
    if (!expect_punct(p, QCC_PUNCT_LPAREN, "expected '(' after 'for'")) {
        return NULL;
    }

    /* Clause 1: a declaration (consuming its ';'), an expression ';', or empty. */
    qcc_stmt *init = NULL;
    if (at_punct(p, QCC_PUNCT_SEMI)) {
        advance(p);
    } else if (qcc_parser_at_declaration(p)) {
        init = parse_decl_stmt(p);
        if (init == NULL) {
            return NULL;
        }
    } else {
        const qcc_token *loc = peek(p);
        qcc_expr        *e   = parse_expression(p);
        if (e == NULL) {
            return NULL;
        }
        if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after for-init")) {
            return NULL;
        }
        init = qcc_stmt_expr(p->ast, e, loc);
        if (init == NULL) {
            return fail_oom_stmt(p);
        }
    }

    /* Clause 2: the controlling expression, optional. */
    qcc_expr *cond = NULL;
    if (!at_punct(p, QCC_PUNCT_SEMI)) {
        cond = parse_expression(p);
        if (cond == NULL) {
            return NULL;
        }
    }
    if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after for-condition")) {
        return NULL;
    }

    /* Clause 3: the iteration expression, optional. */
    qcc_expr *post = NULL;
    if (!at_punct(p, QCC_PUNCT_RPAREN)) {
        post = parse_expression(p);
        if (post == NULL) {
            return NULL;
        }
    }
    if (!expect_punct(p, QCC_PUNCT_RPAREN, "expected ')' after for-clauses")) {
        return NULL;
    }

    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_for(p->ast, init, cond, post, body, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* for-statement (§6.8.5.3): a block scope wraps the clauses and body so a
   declaration in clause 1 is scoped to the loop. */
static qcc_stmt *parse_for_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* for */
    if (qcc_symtab_push_scope(p->syms, QCC_SCOPE_BLOCK) != QCC_OK) {
        return fail_oom_stmt(p);
    }
    qcc_stmt *s = parse_for_inner(p, kw);
    qcc_symtab_pop_scope(p->syms);
    return s;
}

/* goto-statement (§6.8.6.1). */
static qcc_stmt *parse_goto_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* goto */
    const qcc_token *id = peek(p);
    if (id->kind != QCC_TOKEN_IDENTIFIER) {
        parse_error(p, id, "expected a label name after 'goto'");
        return NULL;
    }
    advance(p);
    if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after goto statement")) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_goto(p->ast, id->spelling, id->spelling_len, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* break / continue statements (§6.8.6.3, §6.8.6.2). */
static qcc_stmt *parse_break_or_continue(qcc_parser *p, int is_break)
{
    const qcc_token *kw = peek(p);
    advance(p); /* break / continue */
    if (!expect_punct(p, QCC_PUNCT_SEMI,
                      is_break ? "expected ';' after 'break'"
                               : "expected ';' after 'continue'")) {
        return NULL;
    }
    qcc_stmt *s =
        is_break ? qcc_stmt_break(p->ast, kw) : qcc_stmt_continue(p->ast, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* return-statement (§6.8.6.4); the value is optional. */
static qcc_stmt *parse_return_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* return */
    qcc_expr *value = NULL;
    if (!at_punct(p, QCC_PUNCT_SEMI)) {
        value = parse_expression(p);
        if (value == NULL) {
            return NULL;
        }
    }
    if (!expect_punct(p, QCC_PUNCT_SEMI, "expected ';' after return statement")) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_return(p->ast, value, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* An identifier-labeled statement (§6.8.1); at_label confirmed the ':' ahead. */
static qcc_stmt *parse_label_statement(qcc_parser *p)
{
    const qcc_token *id = peek(p);
    advance(p); /* identifier */
    advance(p); /* ':' */
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_label(p->ast, id->spelling, id->spelling_len, body, id);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* case-labeled statement (§6.8.1); the label is a constant-expression (§6.6). */
static qcc_stmt *parse_case_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* case */
    qcc_expr *value = parse_conditional(p);
    if (value == NULL) {
        return NULL;
    }
    if (!expect_punct(p, QCC_PUNCT_COLON, "expected ':' after case label")) {
        return NULL;
    }
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_case(p->ast, value, body, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* default-labeled statement (§6.8.1). */
static qcc_stmt *parse_default_statement(qcc_parser *p)
{
    const qcc_token *kw = peek(p);
    advance(p); /* default */
    if (!expect_punct(p, QCC_PUNCT_COLON, "expected ':' after 'default'")) {
        return NULL;
    }
    qcc_stmt *body = parse_statement(p);
    if (body == NULL) {
        return NULL;
    }
    qcc_stmt *s = qcc_stmt_default(p->ast, body, kw);
    return (s != NULL) ? s : fail_oom_stmt(p);
}

/* statement (§6.8): dispatch on the leading token. */
static qcc_stmt *parse_statement(qcc_parser *p)
{
    const qcc_token *t = peek(p);

    if (at_punct(p, QCC_PUNCT_LBRACE)) {
        return parse_compound_statement(p);
    }
    if (t->kind == QCC_TOKEN_KEYWORD) {
        switch (t->keyword) {
        case QCC_KW_IF:       return parse_if_statement(p);
        case QCC_KW_SWITCH:   return parse_switch_statement(p);
        case QCC_KW_WHILE:    return parse_while_statement(p);
        case QCC_KW_DO:       return parse_do_statement(p);
        case QCC_KW_FOR:      return parse_for_statement(p);
        case QCC_KW_GOTO:     return parse_goto_statement(p);
        case QCC_KW_BREAK:    return parse_break_or_continue(p, 1);
        case QCC_KW_CONTINUE: return parse_break_or_continue(p, 0);
        case QCC_KW_RETURN:   return parse_return_statement(p);
        case QCC_KW_CASE:     return parse_case_statement(p);
        case QCC_KW_DEFAULT:  return parse_default_statement(p);
        default:              break; /* Other keywords begin an expression. */
        }
    }
    if (at_label(p)) {
        return parse_label_statement(p);
    }
    return parse_expression_statement(p);
}

qcc_status qcc_parse_statement(qcc_parser *p, qcc_stmt **out)
{
    if (p == NULL || out == NULL || p->types == NULL || p->syms == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;

    qcc_stmt *s = parse_statement(p);
    if (p->oom) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    if (s == NULL || p->had_error) {
        return QCC_ERR_PARSE;
    }
    *out = s;
    return QCC_OK;
}

/*
 * External definitions (§6.9; ADR-0024 Unit 4). A translation unit is the loop of
 * qcc_parse_external_declaration over the token stream. Each external declaration
 * is either a function definition or an ordinary declaration; they share the same
 * declaration-specifiers + declarator prefix and diverge only at the token after
 * the first declarator (a '{' begins a function body).
 */

/* Move a parsed init-declarator list into an external-declaration result (an
   arena-owned copy of the decls), then dispose the transient list. */
static qcc_status decls_into_extern(qcc_parser *p, qcc_decl_list *decls,
                                    qcc_extern_decl *out)
{
    out->is_function = 0;
    out->decls       = NULL;
    out->decl_count  = 0;
    if (decls->count != 0) {
        qcc_decl *owned = (qcc_decl *)qcc_ast_dup(p->ast, decls->items,
                                                  decls->count * sizeof(*owned));
        if (owned == NULL) {
            qcc_decl_list_dispose(decls);
            p->oom = 1;
            return QCC_ERR_OUT_OF_MEMORY;
        }
        out->decls      = owned;
        out->decl_count = decls->count;
    }
    qcc_decl_list_dispose(decls);
    return QCC_OK;
}

/* Parse a whole declaration at the cursor (specifiers included) into `out`. Used
   for the static_assert-declaration, which has no declaration-specifiers. */
static qcc_status parse_declaration_into_extern(qcc_parser *p,
                                                qcc_extern_decl *out)
{
    qcc_decl_list decls;
    qcc_decl_list_init(&decls);
    qcc_status st = qcc_parse_declaration(p, &decls);
    if (st != QCC_OK) {
        qcc_decl_list_dispose(&decls);
        return st;
    }
    return decls_into_extern(p, &decls, out);
}

/* Finish an ordinary declaration whose specifiers were already parsed (`ds`/`base`),
   storing its init-declarators in `out`. The no-declarator and non-function paths
   share this so the specifiers — and an enum specifier's registrations (ADR-0026) —
   are parsed exactly once. */
static qcc_status init_declarators_into_extern(qcc_parser *p, const declspec *ds,
                                               const qcc_type *base,
                                               const qcc_token *specloc,
                                               qcc_extern_decl *out)
{
    qcc_decl_list decls;
    qcc_decl_list_init(&decls);
    qcc_status st = parse_init_declarators(p, ds, base, specloc, &decls);
    if (st != QCC_OK) {
        qcc_decl_list_dispose(&decls);
        return st;
    }
    return decls_into_extern(p, &decls, out);
}

/* The body of a function definition: the function name is registered at file
   scope (so the body may recurse), a block scope is opened binding the parameters
   (§6.2.1, §6.9.1), and the brace-delimited block is parsed in that scope (its top
   level shares the parameter scope, so a body declaration cannot reuse a parameter
   name). `name`/`full`/`params`/`param_count`/`ds` come from the declarator. */
static qcc_status parse_function_definition(qcc_parser *p, const declspec *ds,
                                            const qcc_token *name,
                                            const qcc_type *full,
                                            const qcc_param *params,
                                            size_t param_count,
                                            qcc_extern_decl *out)
{
    if (qcc_symtab_insert(p->syms, name->spelling, name->spelling_len,
                          QCC_NS_ORDINARY, QCC_SYM_FUNCTION, full, name->source,
                          name->offset, name->line, name->column, NULL) != QCC_OK) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    if (qcc_symtab_push_scope(p->syms, QCC_SCOPE_BLOCK) != QCC_OK) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < param_count; ++i) {
        if (params[i].name == NULL) {
            continue; /* An unnamed prototype parameter binds nothing. */
        }
        if (qcc_symtab_insert(p->syms, params[i].name, params[i].name_len,
                              QCC_NS_ORDINARY, QCC_SYM_OBJECT, params[i].type,
                              NULL, 0, 0, 0, NULL) != QCC_OK) {
            qcc_symtab_pop_scope(p->syms);
            return QCC_ERR_OUT_OF_MEMORY;
        }
    }

    const qcc_token *lbrace = peek(p); /* the '{' confirmed by the caller */
    qcc_stmt        *body   = parse_block_items(p, lbrace);
    qcc_symtab_pop_scope(p->syms);
    if (body == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }

    out->is_function       = 1;
    out->func.storage      = ds->storage;
    out->func.func_spec    = ds->func_spec;
    out->func.type         = full;
    out->func.name         = name->spelling;
    out->func.name_len     = name->spelling_len;
    out->func.params       = params;
    out->func.param_count  = param_count;
    out->func.body         = body;
    out->func.source       = name->source;
    out->func.offset       = name->offset;
    out->func.line         = name->line;
    out->func.column       = name->column;
    return p->had_error ? QCC_ERR_PARSE : QCC_OK;
}

qcc_status qcc_parse_external_declaration(qcc_parser *p, qcc_extern_decl *out)
{
    if (p == NULL || out == NULL || p->types == NULL || p->syms == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    /* A static_assert-declaration (§6.7.10) is the one external declaration with
       no declaration-specifiers; route it through the declaration parser. */
    if (at_keyword(p, QCC_KW_STATIC_ASSERT)) {
        return parse_declaration_into_extern(p, out);
    }

    const qcc_token *specloc = peek(p);
    declspec         ds;
    parse_declaration_specifiers(p, &ds);
    if (p->oom) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    const qcc_type *base = ds_base_type(p, &ds, specloc);
    if (base == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }
    size_t decl_start = p->pos; /* where the declarators begin (after specifiers) */

    /* A declaration with no declarators (e.g. `struct foo;`, `enum E { A };`). */
    if (at_punct(p, QCC_PUNCT_SEMI)) {
        return init_declarators_into_extern(p, &ds, base, specloc, out);
    }

    /* Speculatively parse the first declarator — side-effect-free apart from the
       cursor — to learn whether a function body follows, capturing parameter names
       in case it does. */
    p->capture_params  = 1;
    p->cap_params      = NULL;
    p->cap_param_count = 0;
    const qcc_token *name        = NULL;
    const qcc_type  *full        = parse_declarator(p, base, 0, &name);
    const qcc_param *params      = p->cap_params;
    size_t           param_count = p->cap_param_count;
    p->capture_params = 0;
    if (full == NULL) {
        return p->oom ? QCC_ERR_OUT_OF_MEMORY : QCC_ERR_PARSE;
    }

    if (name != NULL && full->kind == QCC_TYPE_FUNCTION &&
        at_punct(p, QCC_PUNCT_LBRACE)) {
        return parse_function_definition(p, &ds, name, full, params, param_count,
                                         out);
    }

    /* Not a function definition. Rewind to the declarators (not to the start: the
       specifiers were parsed once and an enum definition has already registered its
       tag and constants, ADR-0026) and parse the init-declarator list. */
    p->pos = decl_start;
    return init_declarators_into_extern(p, &ds, base, specloc, out);
}
