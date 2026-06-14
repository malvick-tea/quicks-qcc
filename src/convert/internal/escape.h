/*
 * qcc — convert internals: escape-sequence decoding (ISO C11 §6.4.4.4, §6.4.5)
 *
 * Responsibility
 * Decode one escape sequence in a character-constant or string-literal body to a
 * single code value: the simple escapes (\' \" \? \\ \a \b \f \n \r \t \v), the
 * octal (\ooo, up to three octal digits) and hexadecimal (\xh… , greedy) escapes
 * of §6.4.4.4 ¶4, and the universal character names \uXXXX / \UXXXXXXXX of
 * §6.4.3. Shared by char-constant and string-literal evaluation.
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_ESCAPE_H
#define QCC_CONVERT_INTERNAL_ESCAPE_H

#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"

/*
 * What an escape decoded to, which decides how the value path uses it (ADR-0018):
 *   QCC_ESCAPE_VALUE — a simple (\n), octal (\ooo), or hexadecimal (\xh…) escape
 *                      (§6.4.4.4 ¶4): a raw code-unit value, never re-encoded;
 *                      range-checked against the element type.
 *   QCC_ESCAPE_UCN   — a universal character name \uXXXX / \UXXXXXXXX (§6.4.3): a
 *                      Unicode code point, encoded into the execution character
 *                      set (UTF-8 narrow, UTF-16/UTF-32 wide).
 */
typedef enum qcc_escape_kind {
    QCC_ESCAPE_VALUE = 0,
    QCC_ESCAPE_UCN
} qcc_escape_kind;

/*
 * Decode the escape sequence in `body[0..len)` beginning at *pos, which must
 * index a backslash. Writes the decoded code value to *out_value, the escape's
 * kind to *out_kind (which the caller uses to choose raw-copy vs encode), and
 * advances *pos past the whole sequence. A malformed escape (\x with no hex
 * digit, a short UCN, an out-of-range or surrogate UCN) or an unknown escape
 * letter is reported to `diags` at `src`/`offset`; a best-effort value is still
 * produced. Returns QCC_OK or a hard fault (QCC_ERR_OUT_OF_MEMORY while
 * diagnosing).
 */
qcc_status qcc_decode_escape(const char *body, size_t len, size_t *pos,
                             const qcc_source *src, size_t offset,
                             qcc_diag_sink *diags, uint32_t *out_value,
                             qcc_escape_kind *out_kind);

#endif /* QCC_CONVERT_INTERNAL_ESCAPE_H */
