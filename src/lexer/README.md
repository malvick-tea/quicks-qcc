# module: lexer

**Responsibility:** ISO C11 translation phases 1-3 (§5.1.1.2): turn the physical
bytes of a `qcc_source` into the preprocessing-token stream of §6.4 — line
splicing everywhere (phase 2), comments to one space and newline tokens
(phase 3), maximal-munch tokenization (§6.4 ¶4) including digraphs, literal
prefixes (`L`/`u`/`U`/`u8`), UCNs in identifiers/pp-numbers, and the contextual
header-name mode (§6.4.7) the preprocessor toggles.

**Public interface:** `lexer/lexer.h` (`qcc_lexer`, `qcc_lexer_init`,
`qcc_lexer_next`, `qcc_lexer_set_header_mode`, `qcc_lexer_token_spelling`).

**Internal layout** (ADR-0008, one concern per translation unit):
`internal/cursor` — phases 1-2: the splice-skipping logical reader, character
classes, splice-free spelling copier (the only code that touches bytes);
`internal/scan` — phase-3 scanners with internal structure: identifier/UCN with
literal-prefix morphing, pp-number, char/string bodies, header-names;
`internal/punct` — maximal-munch punctuator scanner + the "other" catch-all;
`lexer.c` — the driver: whitespace/comments/newlines, EOF repair, dispatch, and
the single place where scanner extents become tokens and lexer state moves.

**Key invariants:** every byte read goes through the splice-skipping logical
reader, so phase 2 exists in exactly one place; tokens carry *physical* spans
(diagnostics point at real bytes) and spellings are recovered splice-free via
`qcc_lexer_token_spelling`; the stream always ends NEWLINE-then-EOF (a missing
final newline is repaired, once); lexical errors become diagnostics and lexing
continues — `qcc_lexer_next` fails only on hard faults.

**Documented deviation:** trigraphs (§5.2.1.1) are diagnosed (warning) and left
untranslated, per
[ADR-0013](../../../Quicks-Meta/docs/adr/0013-qcc-pipeline-architecture.md).

**Dependencies:** `status`, `source`, `diag`, `token`.
