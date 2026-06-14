/*
 * qcc — abstract syntax tree (ISO C11 §6.5-6.9; design in ADR-0019)
 *
 * Responsibility
 * Model the parse tree the parser builds and the later stages walk. This header
 * defines the expression node `qcc_expr` (§6.5) — the first delivery (ADR-0019
 * Unit 1) — and the `qcc_ast` arena that owns the nodes. Declarations, statements,
 * and types grow new node kinds in later units; because operators reuse the
 * punctuator enum and leaves embed their token, those additions stay localized.
 *
 * Node model (and why a flat tagged node)
 *   One `qcc_expr` struct, tagged by `kind`, with the per-kind fields documented
 *   field by field — the same "valid iff kind == K" style `qcc_token` (ADR-0017)
 *   and `qcc_ptok` (ADR-0014) use. C has no inheritance; a tagged node walked by a
 *   single switch is the idiom, keeps every node trivially arena-allocated, and
 *   avoids a class hierarchy. Operators are identified by their `qcc_punct`
 *   enumerator (in §6.5 the punctuators are the operators), so there is no second
 *   operator enum to keep in lockstep.
 *
 * Ownership
 *   A `qcc_ast` owns an arena; every node and every child array lives in it and
 *   dies when the ast is disposed. A node BORROWS the spellings/values of the
 *   token(s) it was built from (a leaf embeds its `qcc_token`, whose `spelling`/
 *   `str_data`/`source` point into the producing `convert`): that `convert` — and
 *   the `qcc_source`s behind it — must outlive the ast, exactly as a token's
 *   `source` must outlive the token.
 *
 * Standard: ISO/IEC 9899 (C11) §6.5. Builds on `token`, `arena`, `status`.
 */
#ifndef QCC_AST_AST_H
#define QCC_AST_AST_H

#include <stddef.h>
#include <stdint.h>

#include "arena/arena.h"
#include "status/status.h"
#include "token/token.h"
#include "type/type.h"

/*
 * The kind of an expression node (§6.5). Postfix `++`/`--` are a distinct kind
 * from prefix (which is QCC_EXPR_UNARY) because their semantics differ; both
 * record which operator in `op`. The type-name-dependent forms (cast,
 * sizeof(type-name), _Alignof, compound literal, _Generic) arrive with the
 * declaration parser (ADR-0019 Unit 2) and are not modeled here yet.
 */
typedef enum qcc_expr_kind {
    QCC_EXPR_IDENT = 0,    /* §6.5.1: an identifier (an unresolved name).        */
    QCC_EXPR_INT_CONST,    /* §6.5.1/§6.4.4.1: integer constant.                 */
    QCC_EXPR_FLOAT_CONST,  /* §6.5.1/§6.4.4.2: floating constant.                */
    QCC_EXPR_CHAR_CONST,   /* §6.5.1/§6.4.4.4: character constant.               */
    QCC_EXPR_STRING,       /* §6.5.1/§6.4.5: string literal.                     */
    QCC_EXPR_INDEX,        /* §6.5.2.1: a[b] — `a` base, `b` subscript.          */
    QCC_EXPR_CALL,         /* §6.5.2.2: a(args) — `a` callee, `args` arguments.  */
    QCC_EXPR_MEMBER,       /* §6.5.2.3: a.m / a->m — `a` base, `member` name.    */
    QCC_EXPR_POSTFIX,      /* §6.5.2.4: a++ / a-- — `a` operand, `op` the punct. */
    QCC_EXPR_UNARY,        /* §6.5.3: ++a --a & * + - ~ ! a — `a`, `op`.         */
    QCC_EXPR_SIZEOF,       /* §6.5.3.4: sizeof of an expression — `a` operand.   */
    QCC_EXPR_SIZEOF_TYPE,  /* §6.5.3.4: sizeof ( type-name ) — `type_operand`.   */
    QCC_EXPR_ALIGNOF_TYPE, /* §6.5.3.4: _Alignof ( type-name ) — `type_operand`. */
    QCC_EXPR_CAST,         /* §6.5.4: ( type-name ) a — `type_operand`, `a`.     */
    QCC_EXPR_BINARY,       /* §6.5.5-6.5.14: a op b — `op` the operator punct.   */
    QCC_EXPR_CONDITIONAL,  /* §6.5.15: a ? b : c.                                */
    QCC_EXPR_ASSIGN,       /* §6.5.16: a op b (op = = or a compound assignment). */
    QCC_EXPR_COMMA         /* §6.5.17: a , b.                                     */
} qcc_expr_kind;

/*
 * One expression node. A heap-of-arena object: never freed individually; the
 * owning qcc_ast frees the whole tree at once. Fields are valid per `kind`:
 *
 *   op             : UNARY/POSTFIX/BINARY/ASSIGN — the operator's punctuator.
 *   tok            : the leaf's token (IDENT/INT_CONST/FLOAT_CONST/CHAR_CONST/
 *                    STRING) — its spelling and the value `convert` computed.
 *   a              : the (only/first) child — UNARY/POSTFIX/SIZEOF operand;
 *                    BINARY/ASSIGN/COMMA left; INDEX base; CALL callee; MEMBER
 *                    base; CONDITIONAL condition.
 *   b              : the second child — BINARY/ASSIGN/COMMA right; INDEX
 *                    subscript; CONDITIONAL "then".
 *   c              : the third child — CONDITIONAL "else".
 *   args/arg_count : CALL argument list (each a full expression), in order.
 *   member/_len    : MEMBER — the member name (borrowed token spelling).
 *   is_arrow       : MEMBER — 1 for `->`, 0 for `.`.
 *   source/offset/line/column : provenance for diagnostics (the operator token
 *                    for compound nodes, the leaf token for leaves).
 */
typedef struct qcc_expr qcc_expr;
struct qcc_expr {
    qcc_expr_kind            kind;
    qcc_punct                op;

    qcc_token                tok;
    const qcc_type          *type_operand; /* CAST/SIZEOF_TYPE/ALIGNOF_TYPE.     */

    qcc_expr                *a;
    qcc_expr                *b;
    qcc_expr                *c;
    qcc_expr               **args;
    size_t                   arg_count;

    const char              *member;
    size_t                   member_len;
    int                      is_arrow;

    const struct qcc_source *source;
    size_t                   offset;
    uint32_t                 line;
    uint32_t                 column;
};

/*
 * The tree's owner: an arena backing every node. Treat the fields as private; use
 * the functions. Lives on the stack; only the arena's blocks are heap-owned.
 */
typedef struct qcc_ast {
    qcc_arena arena;
} qcc_ast;

/* Initialize an empty ast (lazy arena). Always succeeds; pass to dispose even on
   an early return. */
qcc_status qcc_ast_init(qcc_ast *ast);

/* Release the arena. After this every node it produced is invalid. Idempotent
   and NULL-safe. */
void qcc_ast_dispose(qcc_ast *ast);

/*
 * Node constructors. Each allocates from the ast's arena and returns the node,
 * or NULL on out-of-memory (the caller maps that to QCC_ERR_OUT_OF_MEMORY). The
 * `loc` token supplies the node's provenance and is not retained beyond copying
 * its location (leaves copy the whole token).
 */

/* A primary leaf from a converted token; `kind` is derived from tok->kind
   (IDENTIFIER→IDENT, INTEGER→INT_CONST, …). `tok` must be one of those kinds. */
qcc_expr *qcc_expr_leaf(qcc_ast *ast, const qcc_token *tok);

qcc_expr *qcc_expr_unary(qcc_ast *ast, qcc_punct op, qcc_expr *operand,
                         const qcc_token *loc);
qcc_expr *qcc_expr_postfix(qcc_ast *ast, qcc_punct op, qcc_expr *operand,
                           const qcc_token *loc);
qcc_expr *qcc_expr_sizeof(qcc_ast *ast, qcc_expr *operand, const qcc_token *loc);
/* sizeof / _Alignof applied to a type-name (§6.5.3.4), and a cast (§6.5.4). */
qcc_expr *qcc_expr_sizeof_type(qcc_ast *ast, const qcc_type *type,
                               const qcc_token *loc);
qcc_expr *qcc_expr_alignof_type(qcc_ast *ast, const qcc_type *type,
                                const qcc_token *loc);
qcc_expr *qcc_expr_cast(qcc_ast *ast, const qcc_type *type, qcc_expr *operand,
                        const qcc_token *loc);
qcc_expr *qcc_expr_binary(qcc_ast *ast, qcc_punct op, qcc_expr *lhs,
                          qcc_expr *rhs, const qcc_token *loc);
qcc_expr *qcc_expr_assign(qcc_ast *ast, qcc_punct op, qcc_expr *lhs,
                          qcc_expr *rhs, const qcc_token *loc);
qcc_expr *qcc_expr_conditional(qcc_ast *ast, qcc_expr *cond, qcc_expr *then_e,
                               qcc_expr *else_e, const qcc_token *loc);
qcc_expr *qcc_expr_comma(qcc_ast *ast, qcc_expr *lhs, qcc_expr *rhs,
                         const qcc_token *loc);
qcc_expr *qcc_expr_index(qcc_ast *ast, qcc_expr *base, qcc_expr *subscript,
                         const qcc_token *loc);
qcc_expr *qcc_expr_member(qcc_ast *ast, qcc_expr *base, int is_arrow,
                          const char *name, size_t name_len,
                          const qcc_token *loc);
/* Copies the `count` argument pointers from `args` into the arena. */
qcc_expr *qcc_expr_call(qcc_ast *ast, qcc_expr *callee, qcc_expr *const *args,
                        size_t count, const qcc_token *loc);

/* Stable lowercase name of an expression kind ("ident", "binary", …). */
const char *qcc_expr_kind_name(qcc_expr_kind kind);

/* A storage-class specifier (§6.7.1 ¶1); at most one per declaration (§6.7.1 ¶2,
   except _Thread_local with static/extern). QCC_SC_NONE means none was given. */
typedef enum qcc_storage_class {
    QCC_SC_NONE = 0,
    QCC_SC_TYPEDEF,       /* typedef        */
    QCC_SC_EXTERN,        /* extern         */
    QCC_SC_STATIC,        /* static         */
    QCC_SC_THREAD_LOCAL,  /* _Thread_local  */
    QCC_SC_AUTO,          /* auto           */
    QCC_SC_REGISTER       /* register       */
} qcc_storage_class;

/* Function-specifier bits (§6.7.4 ¶1), OR-combined. */
enum {
    QCC_FS_NONE     = 0,
    QCC_FS_INLINE   = 1u << 0, /* inline     */
    QCC_FS_NORETURN = 1u << 1  /* _Noreturn  */
};

/*
 * One declared entity — a single init-declarator of a declaration (§6.7). A
 * declaration like `int a, *b = 0;` yields several. A plain value (copied into a
 * qcc_decl_list); its `type` is borrowed from a qcc_type_ctx and `init`/`name`
 * from the ast/tokens.
 *
 *   storage    : the storage-class specifier (QCC_SC_NONE if none).
 *   func_spec  : function-specifier bits (QCC_FS_*).
 *   type       : the full declared type (after the declarator is applied).
 *   name/_len  : the declared identifier (NULL/0 for an abstract declarator).
 *   init       : the initializer expression, or NULL.
 *   source/... : provenance (the declarator's identifier, or the specifiers).
 */
typedef struct qcc_decl {
    qcc_storage_class        storage;
    unsigned                 func_spec;
    const qcc_type          *type;
    const char              *name;
    size_t                   name_len;
    qcc_expr                *init;
    const struct qcc_source *source;
    size_t                   offset;
    uint32_t                 line;
    uint32_t                 column;
} qcc_decl;

/*
 * A growable array of qcc_decl (the init-declarators of one or more declarations).
 * The item array is heap-owned (seed allocator); the decls' types/names/inits are
 * not owned here. Use the functions; treat the fields as private.
 */
typedef struct qcc_decl_list {
    qcc_decl *items;
    size_t    count;
    size_t    capacity;
} qcc_decl_list;

void qcc_decl_list_init(qcc_decl_list *list);
void qcc_decl_list_dispose(qcc_decl_list *list);
qcc_status qcc_decl_list_push(qcc_decl_list *list, const qcc_decl *decl);

/* Stable lowercase spelling of a storage class ("typedef", "static", "" for
   QCC_SC_NONE). */
const char *qcc_storage_class_name(qcc_storage_class sc);

/*
 * Render an expression as a parenthesized prefix S-expression — "(+ a (* b c))"
 * for `a + b * c`, "([] x i)" for x[i], "(call f x y)" for f(x, y) — into a newly
 * heap-allocated, NUL-terminated string (*out, owned by the caller: free it) of
 * length *len (excluding the NUL). Deterministic, so tests compare against a
 * literal. Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY / QCC_ERR_INVALID_ARGUMENT.
 */
qcc_status qcc_expr_dump(const qcc_expr *expr, char **out, size_t *len);

#endif /* QCC_AST_AST_H */
