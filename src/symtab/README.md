# module: symtab

**Responsibility:** track declared names and their scopes (ISO C11 §6.2.1,
§6.2.3), so the declaration parser can register identifiers and answer the
question that makes C parseable — is this identifier a `typedef`-name here
(§6.7.8)? Design in
[ADR-0021](../../../Quicks-Meta/docs/adr/0021-qcc-symbol-table-and-scopes.md).

**Public interface:** `symtab/symtab.h` —
- `qcc_symtab`: the table (opens the file scope on init).
- scopes: `qcc_symtab_push_scope` / `qcc_symtab_pop_scope` / `qcc_symtab_depth`
  (scope kinds: file, block, function-prototype, §6.2.1).
- bindings: `qcc_symtab_insert`, `qcc_symtab_lookup` (innermost-out),
  `qcc_symtab_lookup_current_scope` (for redeclaration checks),
  `qcc_symtab_is_typedef_name` (the parser's disambiguation hook).
- `qcc_symbol`: name, name space, kind, `qcc_type`, provenance.
- `qcc_sym_kind_name`.

**Structure:** one arena-owned hash table (FNV-1a, the hash `qas` uses) holds
every live binding at once; each bucket is a chain ordered innermost-first
(insert at head), and each scope threads its own symbols on a list so
`pop_scope` unlinks exactly that scope's bindings. So `lookup` is O(1) expected
and "inner hides outer" (§6.2.1) falls out for free. Keys are
`(name, name space)` — §6.2.3 lets the same identifier be both a tag and an
ordinary identifier (`struct stat stat;`), so the two name spaces (ordinary
identifiers, tags) are distinct; labels and struct members are handled elsewhere.

**Redeclaration policy stays with the parser:** `insert` always inserts (the new
binding shadows any earlier one); the caller checks
`qcc_symtab_lookup_current_scope` first when §6.2.1/§6.7 require diagnosing a
duplicate. This keeps the linkage/compatible-type rules where they belong.

**Ownership:** the table copies each name into its arena, so it outlives the token
stream; a symbol's `type` (from a `qcc_type_ctx`) and `source` are borrowed and
must outlive the table.

**Staged next (ADR-0021):** the label (function-scope) name space and richer
redeclaration/linkage checks arrive when statements and full declarations need
them.

**Dependencies:** `type`, `arena`, `status`.
