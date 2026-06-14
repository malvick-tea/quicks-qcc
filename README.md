# qcc — the C compiler

Part of the **Quicks** toolchain, and the longest pole in the project. `qcc` compiles
**C11** to x86-64 assembly (Intel syntax) consumed by `qas`. It replaces the seed
`gcc`/`clang`, and reaching the point where `qcc` compiles `qcc` (self-hosting) is the
project's central milestone.

> Cross-cutting docs and rationale live in **`Quicks-Meta`** (`../Quicks-Meta`). Relevant:
> [ADR-0006 language baseline](../Quicks-Meta/docs/adr/0006-language-baseline-c11.md) and
> [ADR-0002 self-host then disown](../Quicks-Meta/docs/adr/0002-bootstrap-self-host-then-disown.md).

## Status
**Phase 4 — in progress; the front end runs through preprocessing.** The lexer
(translation phases 1-3, ISO C11 §5.1.1.2 — line splicing everywhere, comments to
one space, maximal-munch pp-tokens with digraphs, literal prefixes, UCNs, and the
contextual header-name mode) feeds a complete **preprocessor** (phase 4, §6.10):
object- and function-like macros with `#`/`##`, variadics and hide-set recursion
control; the predefined macros (§6.10.8); the full conditional-inclusion set with
an integer `#if` evaluator; `#include` with a search path; and `#error`,
`#pragma`, and `#line` — every §6.10 directive. `qcc -E [-I dir] [-iquote dir]
file.c` preprocesses a translation unit and prints the result. All on the
foundation modules (status, source, diag, arena, intern, token), with twelve test
suites. Built with the seed compiler first; then subjected to the
**self-compilation constraint**: `qcc` may use only the C subset `qcc` itself supports.

## Planned pipeline (large-scale by design — ADR/vision "hardest sensible option")
1. **Lexer** → tokens.
2. **Preprocessor** (`#include`, macros, conditionals).
3. **Parser** → typed AST (C11, freestanding subset first).
4. **Semantic analysis** / type checking.
5. **IR in SSA form** + an optimizer pass pipeline (const-fold, DCE, register allocation).
6. **x86-64 backend** → Intel-syntax assembly for `qas`, honoring the System V AMD64
   calling convention.

## Authorities
**ISO/IEC 9899 (C11/C17)** incl. freestanding requirements; System V **AMD64 psABI**
(calling convention, codegen). See `../Quicks-Meta/docs/references/official-specs.md`.
