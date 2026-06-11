# module: token

**Responsibility:** define the *preprocessing-token* value type that flows
between the front-end stages (`qcc_pp_token`, ISO C11 §6.4), the full C11
punctuator set with digraphs folded to primary tokens (§6.4.6), and the 44-entry
C11 keyword table with span lookup (§6.4.1) for phase-7 conversion. Pure data and
total naming/lookup functions; no I/O, no allocation.

**Public interface:** `token/token.h` (`qcc_pp_token`, `qcc_pp_token_kind`,
`qcc_punct`, `qcc_keyword`, `qcc_pp_token_kind_name`, `qcc_punct_str`,
`qcc_keyword_lookup`, `qcc_keyword_str`).

**Key invariants:** tokens are plain values carrying (offset, length) spans into
a `qcc_source` — they own nothing; `punct_spellings[]` order matches the
`qcc_punct` enum exactly; the keyword table is sorted in memcmp order (binary
search relies on it); `QCC_KW_NONE == 0` so `if (kw)` means "is a keyword".

**Why pp-tokens, not tokens:** translation phases 3-6 operate on preprocessing
tokens; only phase 7 makes keywords/constants out of them (§5.1.1.2). Modeling
the distinction now is what lets the preprocessor land without a retrofit
([ADR-0013](../../../Quicks-Meta/docs/adr/0013-qcc-pipeline-architecture.md)).

**Dependencies:** none (pure leaf module).
