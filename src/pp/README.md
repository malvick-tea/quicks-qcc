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
- `internal/macro` — the macro table (object/function-like records; the
  identical-redefinition test of §6.10.3 ¶2).
- `internal/stream` — the token-input stack: lexes a source, materializes the
  lexer's span-backed tokens into `qcc_ptok` (the one place that happens),
  pushes macro replacements for rescanning and (later) `#include`d files, and
  flags whether a token came from a macro expansion.
- `internal/expand` — macro expansion: object- and function-like, argument
  collection (with a variadic tail), argument pre-expansion in isolation
  (§6.10.3.1), Prosser substitution, and hide-set rescan.
- `internal/glue` — the `#` (stringize, §6.10.3.2) and `##` (paste, §6.10.3.3)
  operators; paste re-lexes the concatenation with the real lexer.
- `internal/builtin` — the predefined macros (§6.10.8): `__LINE__`/`__FILE__`
  (tagged builtins, computed per use) and the fixed-value `__STDC__`,
  `__STDC_VERSION__`, `__STDC_HOSTED__`, `__DATE__`, `__TIME__`.
- `internal/directive` — directive recognition/dispatch: `#define` (object- and
  function-like, variadic), `#undef`, the null directive, and the replacement-
  list constraints (§6.10.3.2/.3). Conditionals, `#include`, and
  `#line`/`#error`/`#pragma` land in later steps.
- `internal/cond` — the conditional-inclusion stack (§6.10.1): tracks the nest
  of #if/#ifdef/#ifndef/#elif/#else/#endif groups and answers "are we emitting?".
- `internal/ceval` — the #if/#elif controlling expression: `defined`
  substitution, macro expansion, identifier→0, then an intmax_t/uintmax_t
  recursive-descent evaluator with C precedence, the usual arithmetic
  conversions, and short-circuit &&/||/?:.
- `pp.c` — the line-oriented driver; `ptok_list.c` — the token list.

**Current behavior:** `qcc_pp_run` runs the phase-4 loop: it materializes the
token stream, executes `#define`/`#undef`, and expands object- and function-like
macros — including `#`, `##`, variadic `__VA_ARGS__`, argument pre-expansion, and
line-spanning invocations — with hide-set-based recursion control (self- and
mutually-recursive macros terminate). The predefined macros of §6.10.8 are in
scope from the first line. Conditional inclusion (`#if`/`#ifdef`/`#ifndef`/
`#elif`/`#else`/`#endif`, with a full integer-constant-expression evaluator and
the `defined` operator) selects which groups are processed; skipped groups are
tracked for nesting but their directives are not executed (§6.10.1 ¶6). Newlines
are consumed, not emitted (phase-4 output has none). `#include` and the CLI `-E`
output are the remaining step; each lands with tests and this README updates with
it.

**Key invariants:** the lexer→`qcc_ptok` materialization is the single boundary
between the two token vocabularies; every `qcc_ptok` spelling is interned (equal
spellings share a pointer) and lives in the `qcc_pp`'s arena — tokens produced by
`qcc_pp_run` are valid only until the `qcc_pp` is disposed; the empty hide set is
the NULL pointer.

**Dependencies:** `lexer`, `token`, `source`, `diag`, `arena`, `intern`,
`status`.
