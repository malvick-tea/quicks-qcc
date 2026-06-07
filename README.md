# qcc — the C compiler

Part of the **Quicks** toolchain, and the longest pole in the project. `qcc` compiles
**C11** to x86-64 assembly (Intel syntax) consumed by `qas`. It replaces the seed
`gcc`/`clang`, and reaching the point where `qcc` compiles `qcc` (self-hosting) is the
project's central milestone.

> Cross-cutting docs and rationale live in **`Quicks-Meta`** (`../Quicks-Meta`). Relevant:
> [ADR-0006 language baseline](../Quicks-Meta/docs/adr/0006-language-baseline-c11.md) and
> [ADR-0002 self-host then disown](../Quicks-Meta/docs/adr/0002-bootstrap-self-host-then-disown.md).

## Status
**Phase 4 — not started.** Built with the seed compiler first; then subjected to the
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
