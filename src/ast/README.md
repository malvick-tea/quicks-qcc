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
- node constructors (`qcc_expr_leaf`, `qcc_expr_binary`, `qcc_expr_cast`,
  `qcc_stmt_if`, `qcc_stmt_for`, …), each allocating from the arena and returning
  NULL on out-of-memory.
- `qcc_expr_kind_name` / `qcc_stmt_kind_name`, and `qcc_expr_dump` /
  `qcc_stmt_dump` — deterministic S-expression renderers used by tests and a
  future `-dump-ast`.

**Node model:** one flat `qcc_expr` struct tagged by `kind`, with the per-kind
fields documented "valid iff kind == K" — the same style `qcc_token` and
`qcc_ptok` use, and the idiom for an AST in C (a tagged node walked by one
`switch`, no class hierarchy). Operators are identified by their `qcc_punct`
enumerator (in §6.5 the punctuators *are* the operators), so there is no second
operator enum to keep in lockstep. A leaf embeds its `qcc_token`, carrying the
spelling and the constant value `convert` already computed.

**Current scope (Units 1-3):** the expression grammar (§6.5; ADR-0019) — primary,
postfix (`[]`, call, `.`/`->`, `++`/`--`), unary, cast, the binary cascade,
conditional, assignment, comma; the declared entity `qcc_decl` (§6.7; ADR-0022);
and the statement grammar (§6.8; ADR-0023) — compound, declaration, expression/null,
selection, iteration, jump, and labeled statements. The S-expression dump is
parenthesized prefix: `a + b * c` ⇒ `(+ a (* b c))`, `x[i]` ⇒ `([] x i)`,
`f(x, y)` ⇒ `(call f x y)`, `(int)x` ⇒ `(cast int x)`,
`if (c) x; else y;` ⇒ `(if c (expr x) (expr y))`,
`for (i = 0; i < n; ++i) ;` ⇒ `(for (expr (= i 0)) (< i n) (pre++ i) (empty))`.

**Ownership / lifetime:** the arena owns every node and child array. A node
*borrows* the spellings/values of the token(s) it was built from (a leaf embeds a
`qcc_token` whose `spelling`/`str_data`/`source` point into the producing
`convert`); that `convert` and its sources must outlive the ast — the same
borrowing rule a token has for its `source`. `qcc_expr_dump` returns a fresh
heap string independent of the arena, so a dump survives teardown.

**Staged next (ADR-0019):** Unit 4 — external definitions / function bodies
(§6.9), which combine a declaration with a compound-statement body. Still to grow
here: struct/union/enum *definition* nodes and initializer-list nodes (§6.7.9).

**Dependencies:** `token`, `arena`, `status`.
