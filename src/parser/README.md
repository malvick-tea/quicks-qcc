# module: parser

**Responsibility:** recognise the C11 grammar over the `qcc_token` stream
`convert` produces and build a `qcc_ast` tree (ISO C11 §6.5-6.9). A hand-written
recursive-descent parser with precedence climbing for the operator cascade.
Design and staged delivery in
[ADR-0019](../../../Quicks-Meta/docs/adr/0019-qcc-parser-and-ast.md).

**Public interface:** `parser/parser.h` —
- `qcc_parser`: the parser state over an EOF-terminated token array, building into
  a borrowed `qcc_ast` and reporting to a borrowed `qcc_diag_sink`.
- `qcc_parser_init`, `qcc_parse_expression` (parse one §6.5.17 expression),
  `qcc_parser_at_end`.

**Current scope (ADR-0019 Unit 1):** the whole §6.5 **expression** grammar over
identifiers, constants, and string literals:
- primary (§6.5.1): identifier, integer/floating/character constant, string
  literal, and `( expression )`;
- postfix (§6.5.2): subscript `[]`, function call, member `.`/`->`, and the
  postfix `++`/`--`;
- unary (§6.5.3): prefix `++`/`--`, `& * + - ~ !`, and `sizeof` of an expression;
- the fifteen binary levels (§6.5.5-6.5.14) via one precedence-climbing loop
  driven by a per-operator precedence table (left-associative);
- the conditional operator (§6.5.15, right-associative, full expression in the
  middle), the right-associative assignments (§6.5.16), and the comma operator
  (§6.5.17).

**Not yet (needs the type parser, Unit 2):** cast-expression,
`sizeof(type-name)`, `_Alignof`, compound literals, `_Generic`. Until declared
type names exist, a `(` in a unary position always begins a parenthesised
expression, so the expression grammar is unambiguous.

**Error handling:** a syntax error is reported to the diag sink with a source
location and the parse returns `QCC_ERR_PARSE` (a node-allocation failure returns
`QCC_ERR_OUT_OF_MEMORY`); the two are distinguished by internal flags. Panic-mode
recovery at statement/declaration boundaries arrives with statements (Unit 3); a
single-expression parse stops at the first error.

**Ownership:** the parser borrows the token array (whose spellings the nodes
borrow), the `qcc_ast`, and the diag sink; it owns nothing and needs no disposal.

**Dependencies:** `token`, `ast`, `diag`, `status`.
