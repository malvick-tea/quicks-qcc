# module: ast

**Responsibility:** the parse tree the parser builds and the later stages walk
(ISO C11 §6.5-6.9). Holds the node types and an arena that owns them. Design and
staged delivery in
[ADR-0019](../../../Quicks-Meta/docs/adr/0019-qcc-parser-and-ast.md).

**Public interface:** `ast/ast.h` —
- `qcc_ast`: the owner (an `arena`); `qcc_ast_init` / `qcc_ast_dispose`.
- `qcc_expr` + `qcc_expr_kind`: the expression node (§6.5) and its categories.
- node constructors (`qcc_expr_leaf`, `qcc_expr_binary`, `qcc_expr_call`, …),
  each allocating from the arena and returning NULL on out-of-memory.
- `qcc_expr_kind_name`, and `qcc_expr_dump` — a deterministic S-expression
  renderer used by tests and a future `-dump-ast`.

**Node model:** one flat `qcc_expr` struct tagged by `kind`, with the per-kind
fields documented "valid iff kind == K" — the same style `qcc_token` and
`qcc_ptok` use, and the idiom for an AST in C (a tagged node walked by one
`switch`, no class hierarchy). Operators are identified by their `qcc_punct`
enumerator (in §6.5 the punctuators *are* the operators), so there is no second
operator enum to keep in lockstep. A leaf embeds its `qcc_token`, carrying the
spelling and the constant value `convert` already computed.

**Current scope (ADR-0019 Unit 1):** the §6.5 expression grammar — primary,
postfix (`[]`, call, `.`/`->`, `++`/`--`), unary (`++`/`--`, `& * + - ~ !`,
`sizeof` expr), the binary cascade, conditional, assignment, and comma. The
S-expression dump is parenthesized prefix: `a + b * c` ⇒ `(+ a (* b c))`,
`x[i]` ⇒ `([] x i)`, `f(x, y)` ⇒ `(call f x y)`, `p->m` ⇒ `(-> p m)`,
`a++` ⇒ `(post++ a)`, prefix `++a` ⇒ `(pre++ a)`.

**Ownership / lifetime:** the arena owns every node and child array. A node
*borrows* the spellings/values of the token(s) it was built from (a leaf embeds a
`qcc_token` whose `spelling`/`str_data`/`source` point into the producing
`convert`); that `convert` and its sources must outlive the ast — the same
borrowing rule a token has for its `source`. `qcc_expr_dump` returns a fresh
heap string independent of the arena, so a dump survives teardown.

**Staged next (ADR-0019):** Unit 2 — declarations and types (§6.7) plus the
symbol table for the typedef ambiguity; Unit 3 — statements (§6.8); Unit 4 —
external definitions (§6.9). Each adds node kinds here.

**Dependencies:** `token`, `arena`, `status`.
