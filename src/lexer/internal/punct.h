/*
 * qcc — lexer internals: punctuator scanner (§6.4.6).
 *
 * Responsibility
 * Maximal-munch recognition of every C11 punctuator, digraphs folded to
 * their primary tokens (§6.4.6 ¶3), with the §6.4 ¶1 "other" catch-all for
 * any character that starts no token class at all.
 *
 * Internal header (ADR-0008): only lexer/ files may include it.
 */
#ifndef QCC_LEXER_INTERNAL_PUNCT_H
#define QCC_LEXER_INTERNAL_PUNCT_H

#include <stddef.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"
#include "lexer/internal/scan.h"

/*
 * Scan the punctuator starting at `start`, trying the longest spelling first
 * (§6.4 ¶4: the next pp-token is the longest sequence that could be one). A
 * '?' that opens a trigraph sequence is warned via qcc_lx_warn_trigraph
 * (ADR-0013) and still lexes as '?'. Any character that is no punctuator
 * becomes a one-character "other" pp-token.
 */
qcc_status qcc_lx_scan_punct_or_other(const qcc_source *src,
                                      qcc_diag_sink *diags, size_t start,
                                      qcc_lx_scan *out);

#endif /* QCC_LEXER_INTERNAL_PUNCT_H */
