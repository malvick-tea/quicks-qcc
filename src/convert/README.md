# module: convert

**Responsibility:** ISO C11 translation phases 5-7 entry (§5.1.1.2) — turn the
preprocessing-token stream `pp` produces into the token stream the parser
consumes (§6.4 ¶3). Design and staged delivery in
[ADR-0017](../../../Quicks-Meta/docs/adr/0017-qcc-convert-stage.md).

**Public interface:** `convert/convert.h` —
- `qcc_token_list`: a growable array of `qcc_token` (init/dispose/push/clear).
- `qcc_convert`: the converter object (owns an `arena` + `intern`);
  `qcc_convert_init`, `qcc_convert_dispose`, `qcc_convert_run`.

The token type itself, `qcc_token`, lives in the `token` module (which models
§6.4 both ways, ADR-0013): a category (`QCC_TOKEN_{EOF,KEYWORD,IDENTIFIER,
INTEGER,FLOATING,CHAR,STRING,PUNCT}`), the resolved `keyword`/`punct`, the
interned `spelling`, and provenance.

**Current behavior (Unit A — reclassification):** `qcc_convert_run` maps each
preprocessing token to a token:
- an identifier that spells one of the 44 C11 keywords (§6.4.1) becomes a
  keyword; otherwise an identifier;
- a pp-number (§6.4.8) is classified — by *shape*, not value — as an integer
  (§6.4.4.1) or floating (§6.4.4.2) constant: a `.`, a decimal `e`/`E` exponent,
  or a hex `p`/`P` exponent ⇒ floating;
- character-constant → CHAR, string-literal → STRING, punctuator → PUNCT
  (carrying the enumerator), EOF → EOF;
- a surviving `OTHER` or header-name preprocessing token is a stray token and is
  diagnosed (§6.4 ¶3); newlines do not occur in phase-4 output.

**Staged next (ADR-0017):** Unit B — integer/floating constant value and type
(§6.4.4.1/.2); Unit C — character/string value with escape decoding (§6.4.4.4,
§6.4.5) and the phase-6 concatenation of adjacent string literals. Until then a
constant token carries its lexeme and is told apart by `kind`.

**Key invariants:** output token spellings are interned through the converter's
own `arena`/`intern`, so the token stream owns its spellings independently of the
preprocessor; a token's `source` pointer borrows the original `qcc_source`
(translation unit and `#include`d files), which must outlive the tokens.

**Dependencies:** `pp`, `token`, `arena`, `intern`, `diag`, `status`.
