/*
 * qcc — the parser: tokens to a syntax tree (ISO C11 §6.5-6.9; design ADR-0019)
 *
 * Responsibility
 * Recognise the C11 grammar over the `qcc_token` stream `convert` produces
 * (translation phase 7 output) and build a `qcc_ast` tree. This is a hand-written
 * recursive-descent parser with precedence climbing for the operator cascade
 * (ADR-0019). It is delivered in units, front of the grammar first.
 *
 * Scope so far (ADR-0019 Unit 1, ADR-0022 Unit 2, ADR-0023 Unit 3, ADR-0024 Unit 4)
 *   Unit 1 — the whole §6.5 expression grammar over identifiers, constants, and
 *   string literals: primary, postfix (`[]`, calls, `.`/`->`, `++`/`--`), unary
 *   (`++`/`--`, `& * + - ~ !`, `sizeof`), cast (§6.5.4), the fifteen binary
 *   levels, the conditional operator, the right-associative assignments, and the
 *   comma operator.
 *   Unit 2 — declarations (§6.7): declaration-specifiers, inside-out declarators
 *   (pointer/array/function, nested), typedef-name resolution, and type-names
 *   (§6.7.7). The type-name parser also backs the §6.5 forms that need a type:
 *   the cast-expression and `sizeof(type-name)` / `_Alignof` (§6.5.3.4), chosen
 *   over the expression reading by the §6.7.8 typedef/keyword test, so `(T)x` is a
 *   cast and `(x)y` is not.
 *   Unit 3 — statements (§6.8): compound statements with a block scope per `{ }`,
 *   the declaration-or-statement choice at each block item, expression/null,
 *   selection (`if`/`switch`), iteration (`while`/`do`/`for`), jump
 *   (`goto`/`continue`/`break`/`return`), and labeled (`case`/`default`/identifier)
 *   statements.
 *   Unit 4 — external definitions (§6.9): a translation unit as a sequence of
 *   external declarations, each an ordinary declaration or a function definition
 *   (the function name at file scope, the body a block scope binding the
 *   parameters).
 *   Still deferred: compound literals and `_Generic`, struct/union/enum
 *   *definitions* (only tag references are parsed), initializer lists (§6.7.9),
 *   the K&R definition form, and all semantic analysis.
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

    /* Function-definition parameter capture (§6.9.1; ADR-0024), private. While
       capture_params is set, the declarator parser records the parameter
       names/types of the function-definition declarator into cap_params (arena-
       owned), so the body's scope can bind them. Off for ordinary declarations. */
    int              capture_params;
    const qcc_param *cap_params;
    size_t           cap_param_count;

    /* Set while parsing a function declarator's parameter-type-list (§6.7.6.3),
       private. A struct/union/enum *defined* there has a scope limited to that
       prototype (§6.2.1 ¶4, §6.7.2.3) and is therefore useless; we do not model
       prototype scope, so the enum specifier builds the type but registers nothing
       (ADR-0026). This also keeps the speculative func-detection parse free of
       symbol-table side effects, so re-parsing the declarators cannot double-
       register. Nests (a parameter that is itself a function pointer), so it is
       saved/restored rather than cleared. */
    int              in_param_list;
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

/*
 * Parse one statement (§6.8) at the cursor — a compound statement and its block
 * items, a selection/iteration/jump statement, a labeled statement, or an
 * expression/null statement — leaving the cursor after it. *out receives the tree
 * (allocated from the parser's ast). A compound statement (and a `for` with a
 * declaration in its first clause) opens and closes a block scope, so declaration
 * parsing and the §6.7.8 typedef test work at the right depth; the parser must
 * therefore have a type context and symbol table. Returns QCC_OK; on a syntax
 * error a diagnostic was emitted and the return is QCC_ERR_PARSE;
 * QCC_ERR_OUT_OF_MEMORY on a hard fault; QCC_ERR_INVALID_ARGUMENT if no type
 * ctx/symtab was provided. *out is NULL unless QCC_OK.
 */
qcc_status qcc_parse_statement(qcc_parser *parser, qcc_stmt **out);

/*
 * Parse one external declaration (§6.9) at the cursor — either a function
 * definition (declaration-specifiers, a function declarator, and a compound-
 * statement body) or an ordinary declaration — filling *out (a tagged result; its
 * arrays/body are owned by the parser's ast). A translation unit is the loop of
 * this until qcc_parser_at_end. The function name is registered at file scope and
 * its body parsed in a block scope binding the parameters; requires the parser's
 * type context and symbol table. Returns QCC_OK; on a syntax error a diagnostic
 * was emitted and the return is QCC_ERR_PARSE; QCC_ERR_OUT_OF_MEMORY on a hard
 * fault; QCC_ERR_INVALID_ARGUMENT if no type ctx/symtab was provided.
 */
qcc_status qcc_parse_external_declaration(qcc_parser *parser, qcc_extern_decl *out);

/* Whether the cursor looks at the start of a declaration (a declaration-specifier
   keyword or a visible typedef-name) — the §6.7.8 declaration-vs-expression test. */
int qcc_parser_at_declaration(const qcc_parser *parser);

/* Whether the cursor is at the end-of-stream token (QCC_TOKEN_EOF). */
int qcc_parser_at_end(const qcc_parser *parser);

#endif /* QCC_PARSER_PARSER_H */
