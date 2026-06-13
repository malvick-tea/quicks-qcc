# module: pp

**Responsibility:** ISO C11 translation phase 4 (§6.10) — execute preprocessing
directives and expand macros, turning the lexer's preprocessing-token stream
into the stream the `convert` stage (phase 7) consumes. Design recorded in
[ADR-0014](../../../Quicks-Meta/docs/adr/0014-qcc-preprocessor-design.md).

**Public interface:** `pp/pp.h` —
- `qcc_ptok`: a *materialized* preprocessing token that owns its (interned)
  spelling and records provenance (source, offset, line, column) and a hide set,
  distinct from the lexer's span-backed `qcc_pp_token` (see ADR-0014 for why two
  token types).
- `qcc_ptok_list`: a growable array of `qcc_ptok` (init/dispose/push/clear).
- `qcc_pp`: the preprocessor object (owns an `arena` + `intern`); `qcc_pp_init`,
  `qcc_pp_dispose`, `qcc_pp_intern`, `qcc_pp_run`.

**Internal layout** (ADR-0008, one concern per translation unit; built
front-to-back):
- `internal/hideset` — immutable hide sets of interned macro names (§6.10.3.4):
  the membership/union/intersection algebra that stops self- and mutually-
  recursive macro expansion.
- *(landing next)* `internal/macro`, `internal/stream`, `internal/expand`,
  `internal/cond`, `internal/ceval`, `internal/directive`.
- `pp.c` — the driver and the lexer→`qcc_ptok` materialization boundary;
  `ptok_list.c` — the token list.

**Current behavior:** `qcc_pp_run` materializes the phase-1-3 token stream into
`qcc_ptok` values (interning spellings, recording provenance, empty hide sets).
Directive execution and macro expansion are added through the internal
submodules; each lands with tests and this README is updated in the same commit.

**Key invariants:** the lexer→`qcc_ptok` materialization is the single boundary
between the two token vocabularies; every `qcc_ptok` spelling is interned (equal
spellings share a pointer) and lives in the `qcc_pp`'s arena — tokens produced by
`qcc_pp_run` are valid only until the `qcc_pp` is disposed; the empty hide set is
the NULL pointer.

**Dependencies:** `lexer`, `token`, `source`, `diag`, `arena`, `intern`,
`status`.
