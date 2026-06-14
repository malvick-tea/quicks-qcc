/*
 * qcc — convert internals: string-literal evaluation (ISO C11 §6.4.5, §5.1.1.2
 * phase 6)
 *
 * Responsibility
 * Compute the decoded value of a string literal — or of a run of adjacent string
 * literals, which translation phase 6 concatenates into one (§5.1.1.2, §6.4.5
 * ¶5). For each piece we read its encoding prefix and decode the body (c-chars
 * and escape sequences, via escape.h) into code units of the execution character
 * set (ADR-0018), then append the §6.4.5 ¶6 terminating zero unit. The combined
 * encoding is reconciled across the pieces (§6.4.5 ¶5): an unprefixed piece takes
 * the others' prefix; two different non-empty prefixes are diagnosed.
 *
 * Internal header (ADR-0008): only convert/ files include it.
 */
#ifndef QCC_CONVERT_INTERNAL_STRLIT_H
#define QCC_CONVERT_INTERNAL_STRLIT_H

#include <stddef.h>

#include "arena/arena.h"
#include "diag/diag.h"
#include "pp/pp.h"
#include "status/status.h"
#include "token/token.h"

/*
 * Evaluate the run of `npieces` adjacent string-literal preprocessing tokens
 * `pieces[0..npieces)` (npieces >= 1) into one string value. On success
 * *out_data points at arena-owned storage holding *out_len code units (in the
 * resolved *out_encoding, target/little-endian byte order) followed by one zero
 * terminator unit which *out_len excludes; the unit size is
 * qcc_encoding_unit_size(*out_encoding). The decoded bytes are allocated from
 * `arena`, so they outlive the preprocessor.
 *
 * Malformed escapes, out-of-range numeric escapes, and a mix of differing
 * encoding prefixes are reported to `diags`; a best-effort value is still
 * produced. Returns QCC_OK or QCC_ERR_* for a hard fault (bad argument or OOM).
 */
qcc_status qcc_eval_string(qcc_arena *arena, const qcc_ptok *pieces,
                           size_t npieces, qcc_diag_sink *diags,
                           const void **out_data, size_t *out_len,
                           qcc_char_encoding *out_encoding);

#endif /* QCC_CONVERT_INTERNAL_STRLIT_H */
