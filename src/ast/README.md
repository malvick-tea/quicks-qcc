# module: ast

**Responsibility:** the parse tree the parser builds and the later stages walk
(ISO C11 §6.5-6.9). Holds the node types and an arena that owns them. Design and
staged delivery in
[ADR-0019](../../../Quicks-Meta/docs/adr/0019-qcc-parser-and-ast.md).

**Public interface:** `ast/ast.h` —
- `qcc_ast`: the owner (an `arena`); `qcc_ast_init` / `qcc_ast_dispose`.
- `qcc_expr` + `qcc_expr_kind`: the expression node (§6.5) and its categories,
  including the type-form nodes `cast` / `sizeof(type-name)` / `_Alignof`.
- `qcc_decl` + `qcc_decl_list` + `qcc_storage_class`: a declared entity (§6.7) and
  the growable list one declaration produces.
- `qcc_stmt` + `qcc_stmt_kind`: the statement node (§6.8) and its categories.
- `qcc_param` + `qcc_func_def` + `qcc_extern_decl`: a parameter, a function
  definition, and one external declaration (§6.9).
- node constructors (`qcc_expr_leaf`, `qcc_expr_binary`, `qcc_expr_cast`,
  `qcc_stmt_if`, `qcc_stmt_for`, …), each allocating from the arena and returning
  NULL on out-of-memory; plus `qcc_ast_dup` for moving a transient array into the
  arena.
- `qcc_expr_kind_name` / `qcc_stmt_kind_name`, and `qcc_expr_dump` /
  `qcc_stmt_dump` / `qcc_extern_decl_dump` — deterministic S-expression renderers
  used by tests and a future `-dump-ast`.

**Node model:** one flat `qcc_expr` struct tagged by `kind`, with the per-kind
fields documented "valid iff kind == K" — the same style `qcc_token` and
`qcc_ptok` use, and the idiom for an AST in C (a tagged node walked by one
`switch`, no class hierarchy). Operators are identified by their `qcc_punct`
enumerator (in §6.5 the punctuators *are* the operators), so there is no second
operator enum to keep in lockstep. A leaf embeds its `qcc_token`, carrying the
spelling and the constant value `convert` already computed.

**Current scope (Units 1-4):** the expression grammar (§6.5; ADR-0019) — primary,
postfix (`[]`, call, `.`/`->`, `++`/`--`), unary, cast, the binary cascade,
conditional, assignment, comma; the declared entity `qcc_decl` (§6.7; ADR-0022);
the statement grammar (§6.8; ADR-0023) — compound, declaration, expression/null,
selection, iteration, jump, labeled; and external definitions (§6.9; ADR-0024) —
a function definition `qcc_func_def` or an ordinary declaration, as a
`qcc_extern_decl`. The S-expression dump is parenthesized prefix:
`a + b * c` ⇒ `(+ a (* b c))`, `(int)x` ⇒ `(cast int x)`,
`if (c) x; else y;` ⇒ `(if c (expr x) (expr y))`,
`for (i = 0; i < n; ++i) ;` ⇒ `(for (expr (= i 0)) (< i n) (pre++ i) (empty))`,
`int f(int a){return a;}` ⇒ `(func f (a) (block (return a)))`.

**Ownership / lifetime:** the arena owns every node and child array. A node
*borrows* the spellings/values of the token(s) it was built from (a leaf embeds a
`qcc_token` whose `spelling`/`str_data`/`source` point into the producing
`convert`); that `convert` and its sources must outlive the ast — the same
borrowing rule a token has for its `source`. `qcc_expr_dump` returns a fresh
heap string independent of the arena, so a dump survives teardown.

**Staged next:** semantic analysis (name binding, type checking, constraints) over
this tree — the parser deliberately builds the tree without those checks. Still to
grow here: struct/union/enum *definition* nodes and initializer-list nodes
(§6.7.9), compound-literal and `_Generic` expression nodes.

**Dependencies:** `token`, `arena`, `status`.
