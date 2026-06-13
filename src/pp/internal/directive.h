/*
 * qcc — preprocessor internals: directive parsing and dispatch (ISO C11 §6.10)
 *
 * Responsibility
 * Recognize and execute a preprocessing directive once the driver has read a
 * '#' that begins a line (and did not come from a macro expansion). The
 * directive layer reads the remainder of the logical line from the token stream
 * and acts on it: define/undefine macros, and (as later steps land) conditional
 * inclusion, #include, #line/#error/#pragma. It owns no token output — a
 * directive contributes nothing to the phase-4 token stream except its side
 * effects on macro state and which lines are included.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_DIRECTIVE_H
#define QCC_PP_INTERNAL_DIRECTIVE_H

#include "pp/internal/stream.h"
#include "pp/pp.h"
#include "status/status.h"

/*
 * Execute the directive whose introducing '#' is `hash` (already read from
 * `stream`; used only for diagnostics). Consumes the rest of the logical line,
 * including its terminating newline. User errors are reported as diagnostics and
 * the line is skipped; the return value is QCC_OK in that case. Returns a hard
 * fault (QCC_ERR_OUT_OF_MEMORY) only on allocation failure.
 */
qcc_status qcc_pp_directive(qcc_pp *pp, qcc_pp_stream *stream, const qcc_ptok *hash);

#endif /* QCC_PP_INTERNAL_DIRECTIVE_H */
