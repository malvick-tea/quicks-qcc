# module: parser

**Responsibility:** recognise the C11 grammar over the `qcc_token` stream
`convert` produces and build a `qcc_ast` tree (ISO C11 §6.5-6.9). A hand-written
recursive-descent parser with precedence climbing for the operator cascade.
Design and staged delivery in
[ADR-0019](../../../Quicks-Meta/docs/adr/0019-qcc-parser-and-ast.md).

**Public interface:** `parser/parser.h` —
- `qcc_parser`: the parser state over an EOF-terminated token array, building into
  a borrowed `qcc_ast` and reporting to a borrowed `qcc_diag_sink`; it also borrows
  a `qcc_type_ctx` and `qcc_symtab` for declaration parsing (NULL for
  expression-only use).
- `qcc_parser_init`, `qcc_parse_expression` (parse one §6.5.17 expression),
  `qcc_parse_declaration` (one §6.7 declaration), `qcc_parse_type_name` (a §6.7.7
  type-name), `qcc_parser_at_declaration` (the §6.7.8 declaration-vs-expression
  test), `qcc_parser_at_end`.

**Expression grammar (ADR-0019 Unit 1)** — the whole of §6.5 over identifiers,
constants, and string literals:
- primary (§6.5.1): identifier, integer/floating/character constant, string
  literal, and `( expression )`;
- postfix (§6.5.2): subscript `[]`, function call, member `.`/`->`, and the
  postfix `++`/`--`;
- unary (§6.5.3): prefix `++`/`--`, `& * + - ~ !`, `sizeof` of an expression or of
  a `( type-name )` (§6.5.3.4), and `_Alignof ( type-name )`;
- cast (§6.5.4): `( type-name ) cast-expression`;
- the fifteen binary levels (§6.5.5-6.5.14) via one precedence-climbing loop
  driven by a per-operator precedence table (left-associative);
- the conditional operator (§6.5.15, right-associative, full expression in the
  middle), the right-associative assignments (§6.5.16), and the comma operator
  (§6.5.17).

**Declaration grammar (ADR-0022 Unit 2)** — §6.7: declaration-specifiers parsed as
an unordered keyword multiset (§6.7.2), inside-out declarators (§6.7.6:
pointer/array/function and nested declarators via the two-pass cursor method),
typedef-name resolution and registration (§6.7.8), tag *references* (`struct foo
x;`), scalar `=` initializers, and type-names (§6.7.7). The cast / `sizeof` /
`_Alignof` type forms above are gated on the same typedef/keyword test, so a `(`
opens a type only when a type context exists and a type-name follows.

**Deferred:** compound literals and `_Generic` (§6.5); struct/union/enum
*definitions* and brace/designated initializers (§6.7.9); panic-mode recovery
across multiple declarations.

**Error handling:** a syntax error is reported to the diag sink with a source
location and the parse returns `QCC_ERR_PARSE` (a node-allocation failure returns
`QCC_ERR_OUT_OF_MEMORY`); the two are distinguished by internal flags. Panic-mode
recovery at statement/declaration boundaries arrives with statements (Unit 3); a
single-expression parse stops at the first error.

**Ownership:** the parser borrows the token array (whose spellings the nodes
borrow), the `qcc_ast`, and the diag sink; it owns nothing and needs no disposal.

**Dependencies:** `token`, `ast`, `diag`, `status`.
