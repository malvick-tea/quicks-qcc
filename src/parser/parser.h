/*
 * qcc — the parser: tokens to a syntax tree (ISO C11 §6.5-6.9; design ADR-0019)
 *
 * Responsibility
 * Recognise the C11 grammar over the `qcc_token` stream `convert` produces
 * (translation phase 7 output) and build a `qcc_ast` tree. This is a hand-written
 * recursive-descent parser with precedence climbing for the operator cascade
 * (ADR-0019). It is delivered in units, front of the grammar first; this first
 * unit parses the **expression** grammar (§6.5).
 *
 * Scope of this unit (ADR-0019 Unit 1)
 *   The whole §6.5 expression grammar over identifiers, constants, and string
 *   literals: primary, postfix (`[]`, calls, `.`/`->`, `++`/`--`), unary
 *   (`++`/`--`, `& * + - ~ !`, `sizeof` of an expression), the fifteen binary
 *   levels, the conditional operator, the right-associative assignments, and the
 *   comma operator. The type-name-dependent forms — cast, `sizeof(type-name)`,
 *   `_Alignof`, compound literals, `_Generic` — need the declaration/type parser
 *   and arrive with it (Unit 2); until then a `(` in a unary position always
 *   begins a parenthesised expression, which is unambiguous because no type names
 *   are declared yet.
 *
 * Ownership
 *   A qcc_parser borrows everything: the token array (which must stay valid, and
 *   whose spellings the produced nodes borrow), the qcc_ast that the nodes are
 *   allocated from, and the qcc_diag_sink it reports to. It owns nothing and needs
 *   no disposal. Diagnostics are emitted with source locations; a syntax error is
 *   recoverable in the sense that the sink records it and the call returns
 *   QCC_ERR_PARSE rather than crashing.
 *
 * Standard: ISO/IEC 9899 (C11) §6.5. Builds on `token`, `ast`, `diag`, `status`.
 */
#ifndef QCC_PARSER_PARSER_H
#define QCC_PARSER_PARSER_H

#include <stddef.h>

#include "ast/ast.h"
#include "diag/diag.h"
#include "status/status.h"
#include "symtab/symtab.h"
#include "token/token.h"
#include "type/type.h"

/*
 * The parser state. Treat the fields as private; use the functions. `tokens`
 * points at an EOF-terminated array of `count` tokens (a qcc_token_list's items);
 * `pos` is the cursor. `ast`, `diags`, `types`, and `syms` are borrowed and must
 * outlive the parser. `types`/`syms` may be NULL for expression-only use (Unit 1);
 * declaration parsing requires them.
 */
typedef struct qcc_parser {
    const qcc_token *tokens;
    size_t           count;
    size_t           pos;
    qcc_ast         *ast;     /* Borrowed; nodes are allocated from it.        */
    qcc_type_ctx    *types;   /* Borrowed; types are built here (decls).       */
    qcc_symtab      *syms;    /* Borrowed; names registered/resolved here.     */
    qcc_diag_sink   *diags;   /* Borrowed; syntax errors are reported here.    */
    int              had_error; /* A syntax error was diagnosed.               */
    int              oom;       /* A node allocation failed (hard fault).      */
} qcc_parser;

/*
 * Initialize a parser over `tokens[0..count)` (EOF-terminated; count >= 1),
 * building into `ast` and reporting to `diags` (all non-NULL, must outlive it).
 * `types` and `syms` back declaration parsing; pass NULL for expression-only use.
 * Returns QCC_OK or QCC_ERR_INVALID_ARGUMENT.
 */
qcc_status qcc_parser_init(qcc_parser *parser, const qcc_token *tokens,
                           size_t count, qcc_ast *ast, qcc_type_ctx *types,
                           qcc_symtab *syms, qcc_diag_sink *diags);

/*
 * Parse one expression (§6.5.17, the comma operator at the top) starting at the
 * cursor, leaving the cursor on the first token after it. On success *out is the
 * tree (allocated from the parser's ast) and the return is QCC_OK. On a syntax
 * error a diagnostic has been emitted and the return is QCC_ERR_PARSE; on a node
 * allocation failure it is QCC_ERR_OUT_OF_MEMORY. *out is NULL unless QCC_OK.
 */
qcc_status qcc_parse_expression(qcc_parser *parser, qcc_expr **out);

/*
 * Parse one declaration (§6.7) at the cursor — declaration-specifiers, an
 * init-declarator list, and the closing ';' — appending one qcc_decl per declared
 * name to `out` and registering each name in the symbol table (a typedef as a
 * typedef-name, others as object/function). Requires the parser to have been given
 * a type context and symbol table. On success returns QCC_OK; on a syntax error a
 * diagnostic was emitted and the return is QCC_ERR_PARSE; QCC_ERR_OUT_OF_MEMORY on
 * a hard fault; QCC_ERR_INVALID_ARGUMENT if no type ctx/symtab was provided.
 */
qcc_status qcc_parse_declaration(qcc_parser *parser, qcc_decl_list *out);

/*
 * Parse a type-name (§6.7.7) — specifier-qualifier-list and an optional abstract
 * declarator — at the cursor, leaving the cursor after it. *out receives the type.
 * Returns QCC_OK, QCC_ERR_PARSE, QCC_ERR_OUT_OF_MEMORY, or QCC_ERR_INVALID_ARGUMENT.
 */
qcc_status qcc_parse_type_name(qcc_parser *parser, const qcc_type **out);

/* Whether the cursor looks at the start of a declaration (a declaration-specifier
   keyword or a visible typedef-name) — the §6.7.8 declaration-vs-expression test. */
int qcc_parser_at_declaration(const qcc_parser *parser);

/* Whether the cursor is at the end-of-stream token (QCC_TOKEN_EOF). */
int qcc_parser_at_end(const qcc_parser *parser);

#endif /* QCC_PARSER_PARSER_H */
