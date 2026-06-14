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

**Integer constants (Unit B, §6.4.4.1):** an INTEGER token also carries its
`int_value` and `int_type`. `internal/intconst` parses the base (decimal, octal
`0…`, hex `0x…`) and the `u`/`l`/`ll` suffix by hand (no `strtoull`, so it runs
under the self-hosted toolchain later), detects overflow, and picks the type as
the first of the constant's §6.4.4.1 ¶5 candidate list that holds the value
against the x86-64 LP64 widths (int 32, long/long long 64). Bad digits, invalid
suffixes, and out-of-range values are diagnosed; evaluation is best-effort so
conversion continues.

**Floating constants (§6.4.4.2):** a FLOATING token carries its `float_value` and
`float_type` (double / float / long double, set by the f/l suffix).
`internal/floatconst` reads the suffix and hands the numeric part — decimal or
C99 hex float — to the host `strtod` (a seed-CRT dependency, ADR-0009, to be
replaced by our own correctly-rounded parser when self-hosting needs it);
malformed or out-of-range constants are diagnosed.

**Character constants (Unit C, §6.4.4.4):** a CHAR token carries its `int_value`
and `char_encoding` (the L/u/U prefix; none ⇒ type int). `internal/charconst`
reads the prefix and decodes the body via `internal/escape`. A plain constant's
execution set is UTF-8 (ADR-0018): a single byte is the (sign-extended, since
`char` is signed on the target) value, so `'\xFF'` is −1; more than one byte is an
implementation-defined multi-character constant, packed big-endian into an int
with a warning. A wide/u/U constant is one code unit of the prefix's type
(wchar_t 32-bit, char16_t 16-bit, char32_t 32-bit), range-checked. Empty and
out-of-range constants are diagnosed.

**String literals (§6.4.5) and concatenation (phase 6, §5.1.1.2):** a STRING
token carries `str_data`/`str_len` (the decoded code units, excluding the §6.4.5
¶6 zero terminator that follows them, in target/little-endian order) and
`char_encoding`. `internal/strlit` resolves the combined encoding of a run of
adjacent string-literal tokens (an unprefixed piece takes its neighbours' prefix;
a clash of two non-empty prefixes is diagnosed), decodes each piece into that
encoding — narrow/u8 → UTF-8 bytes, L → UTF-32, u → UTF-16 (surrogate pairs for
non-BMP), U → UTF-32 — and concatenates the values, so `"\101" "B"` is `AB`. A
universal character name is encoded into the execution set; a numeric escape is a
raw, range-checked code unit. `internal/encoding` holds the shared UTF-8/UTF-16
codecs and unit sizes. Encoding model and widths: ADR-0018.

**Key invariants:** output token spellings are interned through the converter's
own `arena`/`intern`, so the token stream owns its spellings independently of the
preprocessor; a token's `source` pointer borrows the original `qcc_source`
(translation unit and `#include`d files), which must outlive the tokens.

**Dependencies:** `pp`, `token`, `arena`, `intern`, `diag`, `status`.
