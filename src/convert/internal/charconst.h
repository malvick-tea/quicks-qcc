/*
 * qcc — convert internals: character-constant evaluation (ISO C11 §6.4.4.4)
 *
 * Responsibility
 * Compute the value and encoding of a character-constant lexeme: read the
 * optional prefix (L → wchar_t, u → char16_t, U → char32_t; none → int), decode
 * the characters between the quotes (with escape sequences, via escape.h), and
 * fold them into the constant's value. A plain single-character constant is the
 * char converted to int — and on the x86-64 target `char` is signed, so a byte
 * with the high bit set sign-extends (e.g. '\xFF' is -1).
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_CHARCONST_H
#define QCC_CONVERT_INTERNAL_CHARCONST_H

#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Evaluate the character-constant lexeme s[0..n). Writes the value to *out_value
 * and the encoding to *out_encoding. An empty constant, a multi-character
 * constant (impl-defined; warned), or a malformed escape is reported to `diags`
 * at `src`/`offset`; a best-effort value is still produced. Returns QCC_OK or a
 * hard fault (QCC_ERR_OUT_OF_MEMORY while diagnosing).
 */
qcc_status qcc_eval_char(const char *s, size_t n, const qcc_source *src,
                         size_t offset, qcc_diag_sink *diags,
                         uint64_t *out_value, qcc_char_encoding *out_encoding);

#endif /* QCC_CONVERT_INTERNAL_CHARCONST_H */
