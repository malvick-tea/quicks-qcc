/*
 * qcc — abstract syntax tree (implementation).
 *
 * See ast.h and ADR-0019. Nodes are arena-allocated and zero-initialized, so a
 * constructor only sets the fields its kind uses; the rest read as NULL/0. The
 * S-expression dumper is a small recursive printer over a growable buffer, used
 * by the parser tests (and a future -dump-ast).
 */
#include "ast/ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

qcc_status qcc_ast_init(qcc_ast *ast)
{
    if (ast == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&ast->arena, 0);
    return QCC_OK;
}

void qcc_ast_dispose(qcc_ast *ast)
{
    if (ast != NULL) {
        qcc_arena_dispose(&ast->arena);
    }
}

/* Allocate a zeroed node of `kind`, stamping its provenance from `loc`. */
static qcc_expr *node_new(qcc_ast *ast, qcc_expr_kind kind, const qcc_token *loc)
{
    qcc_expr *e = (qcc_expr *)qcc_arena_calloc(&ast->arena, 1, sizeof(*e), 0);
    if (e == NULL) {
        return NULL;
    }
    e->kind = kind;
    if (loc != NULL) {
        e->source = loc->source;
        e->offset = loc->offset;
        e->line   = loc->line;
        e->column = loc->column;
    }
    return e;
}

qcc_expr *qcc_expr_leaf(qcc_ast *ast, const qcc_token *tok)
{
    if (ast == NULL || tok == NULL) {
        return NULL;
    }
    qcc_expr_kind kind;
    switch (tok->kind) {
    case QCC_TOKEN_IDENTIFIER: kind = QCC_EXPR_IDENT;       break;
    case QCC_TOKEN_INTEGER:    kind = QCC_EXPR_INT_CONST;   break;
    case QCC_TOKEN_FLOATING:   kind = QCC_EXPR_FLOAT_CONST; break;
    case QCC_TOKEN_CHAR:       kind = QCC_EXPR_CHAR_CONST;  break;
    case QCC_TOKEN_STRING:     kind = QCC_EXPR_STRING;      break;
    default:                   return NULL; /* Not a primary leaf token. */
    }
    qcc_expr *e = node_new(ast, kind, tok);
    if (e != NULL) {
        e->tok = *tok; /* Borrows the token's spelling/value (see ast.h). */
    }
    return e;
}

qcc_expr *qcc_expr_unary(qcc_ast *ast, qcc_punct op, qcc_expr *operand,
                         const qcc_token *loc)
{
    if (ast == NULL || operand == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_UNARY, loc);
    if (e != NULL) {
        e->op = op;
        e->a  = operand;
    }
    return e;
}

qcc_expr *qcc_expr_postfix(qcc_ast *ast, qcc_punct op, qcc_expr *operand,
                           const qcc_token *loc)
{
    if (ast == NULL || operand == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_POSTFIX, loc);
    if (e != NULL) {
        e->op = op;
        e->a  = operand;
    }
    return e;
}

qcc_expr *qcc_expr_sizeof(qcc_ast *ast, qcc_expr *operand, const qcc_token *loc)
{
    if (ast == NULL || operand == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_SIZEOF, loc);
    if (e != NULL) {
        e->a = operand;
    }
    return e;
}

qcc_expr *qcc_expr_binary(qcc_ast *ast, qcc_punct op, qcc_expr *lhs,
                          qcc_expr *rhs, const qcc_token *loc)
{
    if (ast == NULL || lhs == NULL || rhs == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_BINARY, loc);
    if (e != NULL) {
        e->op = op;
        e->a  = lhs;
        e->b  = rhs;
    }
    return e;
}

qcc_expr *qcc_expr_assign(qcc_ast *ast, qcc_punct op, qcc_expr *lhs,
                          qcc_expr *rhs, const qcc_token *loc)
{
    if (ast == NULL || lhs == NULL || rhs == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_ASSIGN, loc);
    if (e != NULL) {
        e->op = op;
        e->a  = lhs;
        e->b  = rhs;
    }
    return e;
}

qcc_expr *qcc_expr_conditional(qcc_ast *ast, qcc_expr *cond, qcc_expr *then_e,
                               qcc_expr *else_e, const qcc_token *loc)
{
    if (ast == NULL || cond == NULL || then_e == NULL || else_e == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_CONDITIONAL, loc);
    if (e != NULL) {
        e->a = cond;
        e->b = then_e;
        e->c = else_e;
    }
    return e;
}

qcc_expr *qcc_expr_comma(qcc_ast *ast, qcc_expr *lhs, qcc_expr *rhs,
                         const qcc_token *loc)
{
    if (ast == NULL || lhs == NULL || rhs == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_COMMA, loc);
    if (e != NULL) {
        e->a = lhs;
        e->b = rhs;
    }
    return e;
}

qcc_expr *qcc_expr_index(qcc_ast *ast, qcc_expr *base, qcc_expr *subscript,
                         const qcc_token *loc)
{
    if (ast == NULL || base == NULL || subscript == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_INDEX, loc);
    if (e != NULL) {
        e->a = base;
        e->b = subscript;
    }
    return e;
}

qcc_expr *qcc_expr_member(qcc_ast *ast, qcc_expr *base, int is_arrow,
                          const char *name, size_t name_len,
                          const qcc_token *loc)
{
    if (ast == NULL || base == NULL || name == NULL) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_MEMBER, loc);
    if (e != NULL) {
        e->a          = base;
        e->is_arrow   = is_arrow ? 1 : 0;
        e->member     = name; /* Borrows the identifier token's spelling. */
        e->member_len = name_len;
    }
    return e;
}

qcc_expr *qcc_expr_call(qcc_ast *ast, qcc_expr *callee, qcc_expr *const *args,
                        size_t count, const qcc_token *loc)
{
    if (ast == NULL || callee == NULL || (count != 0 && args == NULL)) {
        return NULL;
    }
    qcc_expr *e = node_new(ast, QCC_EXPR_CALL, loc);
    if (e == NULL) {
        return NULL;
    }
    e->a = callee;
    if (count != 0) {
        qcc_expr **owned = (qcc_expr **)qcc_arena_memdup(
            &ast->arena, args, count * sizeof(*args), 0);
        if (owned == NULL) {
            return NULL;
        }
        e->args      = owned;
        e->arg_count = count;
    }
    return e;
}

const char *qcc_expr_kind_name(qcc_expr_kind kind)
{
    switch (kind) {
    case QCC_EXPR_IDENT:       return "ident";
    case QCC_EXPR_INT_CONST:   return "int-const";
    case QCC_EXPR_FLOAT_CONST: return "float-const";
    case QCC_EXPR_CHAR_CONST:  return "char-const";
    case QCC_EXPR_STRING:      return "string";
    case QCC_EXPR_INDEX:       return "index";
    case QCC_EXPR_CALL:        return "call";
    case QCC_EXPR_MEMBER:      return "member";
    case QCC_EXPR_POSTFIX:     return "postfix";
    case QCC_EXPR_UNARY:       return "unary";
    case QCC_EXPR_SIZEOF:      return "sizeof";
    case QCC_EXPR_BINARY:      return "binary";
    case QCC_EXPR_CONDITIONAL: return "conditional";
    case QCC_EXPR_ASSIGN:      return "assign";
    case QCC_EXPR_COMMA:       return "comma";
    }
    return "unknown";
}

void qcc_decl_list_init(qcc_decl_list *list)
{
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qcc_decl_list_dispose(qcc_decl_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

qcc_status qcc_decl_list_push(qcc_decl_list *list, const qcc_decl *decl)
{
    if (list == NULL || decl == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (list->count == list->capacity) {
        size_t    ncap  = (list->capacity == 0) ? 8u : list->capacity * 2u;
        qcc_decl *grown = (qcc_decl *)realloc(list->items, ncap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        list->items    = grown;
        list->capacity = ncap;
    }
    list->items[list->count++] = *decl;
    return QCC_OK;
}

const char *qcc_storage_class_name(qcc_storage_class sc)
{
    switch (sc) {
    case QCC_SC_NONE:         return "";
    case QCC_SC_TYPEDEF:      return "typedef";
    case QCC_SC_EXTERN:       return "extern";
    case QCC_SC_STATIC:       return "static";
    case QCC_SC_THREAD_LOCAL: return "_Thread_local";
    case QCC_SC_AUTO:         return "auto";
    case QCC_SC_REGISTER:     return "register";
    }
    return "unknown";
}

/* A growable text buffer for the dumper (seed allocator, transient). */
typedef struct strbuf {
    char  *data;
    size_t len;
    size_t cap;
} strbuf;

static int sb_reserve(strbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) { /* +1 keeps room for the NUL. */
        return 1;
    }
    size_t ncap = (b->cap == 0) ? 64u : b->cap;
    while (ncap < b->len + extra + 1) {
        ncap *= 2u;
    }
    char *grown = (char *)realloc(b->data, ncap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap  = ncap;
    return 1;
}

static int sb_putn(strbuf *b, const char *s, size_t n)
{
    if (!sb_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_puts(strbuf *b, const char *s)
{
    return sb_putn(b, s, strlen(s));
}

static int sb_putc(strbuf *b, char c)
{
    return sb_putn(b, &c, 1);
}

/* Recursively emit `e` as an S-expression. Returns 1 on success, 0 on OOM. */
static int emit(strbuf *b, const qcc_expr *e)
{
    char num[32];

    switch (e->kind) {
    case QCC_EXPR_IDENT:
        return sb_putn(b, e->tok.spelling, e->tok.spelling_len);
    case QCC_EXPR_INT_CONST:
        snprintf(num, sizeof(num), "%llu", (unsigned long long)e->tok.int_value);
        return sb_puts(b, num);
    case QCC_EXPR_FLOAT_CONST:
        snprintf(num, sizeof(num), "%g", e->tok.float_value);
        return sb_puts(b, num);
    case QCC_EXPR_CHAR_CONST:
        snprintf(num, sizeof(num), "(char %lld)",
                 (long long)(int64_t)e->tok.int_value);
        return sb_puts(b, num);
    case QCC_EXPR_STRING:
        return sb_puts(b, "(str)");
    case QCC_EXPR_INDEX:
        return sb_puts(b, "([] ") && emit(b, e->a) && sb_putc(b, ' ') &&
               emit(b, e->b) && sb_putc(b, ')');
    case QCC_EXPR_CALL: {
        if (!sb_puts(b, "(call ") || !emit(b, e->a)) {
            return 0;
        }
        for (size_t i = 0; i < e->arg_count; ++i) {
            if (!sb_putc(b, ' ') || !emit(b, e->args[i])) {
                return 0;
            }
        }
        return sb_putc(b, ')');
    }
    case QCC_EXPR_MEMBER:
        return sb_putc(b, '(') && sb_puts(b, e->is_arrow ? "-> " : ". ") &&
               emit(b, e->a) && sb_putc(b, ' ') &&
               sb_putn(b, e->member, e->member_len) && sb_putc(b, ')');
    case QCC_EXPR_POSTFIX:
        return sb_puts(b, "(post") && sb_puts(b, qcc_punct_str(e->op)) &&
               sb_putc(b, ' ') && emit(b, e->a) && sb_putc(b, ')');
    case QCC_EXPR_UNARY: {
        int prefix_io = (e->op == QCC_PUNCT_PLUS_PLUS ||
                         e->op == QCC_PUNCT_MINUS_MINUS);
        if (!sb_putc(b, '(') ||
            (prefix_io && !sb_puts(b, "pre")) ||
            !sb_puts(b, qcc_punct_str(e->op)) || !sb_putc(b, ' ') ||
            !emit(b, e->a) || !sb_putc(b, ')')) {
            return 0;
        }
        return 1;
    }
    case QCC_EXPR_SIZEOF:
        return sb_puts(b, "(sizeof ") && emit(b, e->a) && sb_putc(b, ')');
    case QCC_EXPR_BINARY:
    case QCC_EXPR_ASSIGN:
        return sb_putc(b, '(') && sb_puts(b, qcc_punct_str(e->op)) &&
               sb_putc(b, ' ') && emit(b, e->a) && sb_putc(b, ' ') &&
               emit(b, e->b) && sb_putc(b, ')');
    case QCC_EXPR_CONDITIONAL:
        return sb_puts(b, "(?: ") && emit(b, e->a) && sb_putc(b, ' ') &&
               emit(b, e->b) && sb_putc(b, ' ') && emit(b, e->c) &&
               sb_putc(b, ')');
    case QCC_EXPR_COMMA:
        return sb_puts(b, "(, ") && emit(b, e->a) && sb_putc(b, ' ') &&
               emit(b, e->b) && sb_putc(b, ')');
    }
    return 0; /* Unreachable for a well-formed node. */
}

qcc_status qcc_expr_dump(const qcc_expr *expr, char **out, size_t *len)
{
    if (expr == NULL || out == NULL || len == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    strbuf b = { NULL, 0, 0 };
    if (!emit(&b, expr) || !sb_reserve(&b, 0)) {
        free(b.data);
        return QCC_ERR_OUT_OF_MEMORY;
    }
    b.data[b.len] = '\0';
    *out = b.data;
    *len = b.len;
    return QCC_OK;
}
