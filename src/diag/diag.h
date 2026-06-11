/*
 * qcc — diagnostics
 *
 * Responsibility
 * Collect human-facing problems (errors, warnings, notes) discovered while
 * processing a source, each anchored to a span of that source, and render them
 * as "file:line:col: severity: message" with a source-line excerpt and a caret.
 *
 * Why diagnostics are separate from `qcc_status`
 *   A status is for the *program* (control flow): "this call failed". A
 *   diagnostic is for the *human* (a message they can act on). Keeping them apart
 *   lets a stage keep going after a recoverable error and report *several*
 *   problems in one run, instead of dying on the first — the behavior users
 *   expect from an assembler/compiler. (See error-handling.md.)
 *
 * Ownership
 *   The sink owns the formatted message strings it stores and frees them on
 *   dispose. It does NOT own the qcc_source a diagnostic points at; the source
 *   must outlive the sink.
 *
 * Standard: ISO/IEC 9899 (C11), portable subset (ADR-0009). Uses <stdarg.h> for
 * printf-style message formatting and <stdio.h> FILE for output.
 */
#ifndef QCC_DIAG_DIAG_H
#define QCC_DIAG_DIAG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "source/source.h"
#include "status/status.h"

/* Ordered by increasing seriousness; values are used to index counters. */
typedef enum qcc_diag_severity {
    QCC_DIAG_NOTE = 0,
    QCC_DIAG_WARNING,
    QCC_DIAG_ERROR,
    QCC_DIAG_SEVERITY_COUNT /* Sentinel = number of severities (for counter array). */
} qcc_diag_severity;

/* One recorded diagnostic. `message` is owned by the sink; `source` is borrowed. */
typedef struct qcc_diag {
    qcc_diag_severity  severity;
    const qcc_source  *source;  /* Borrowed; must outlive the sink. May be NULL.  */
    size_t             offset;   /* Byte offset of the span start in `source`.     */
    size_t             length;   /* Span length in bytes (>= 1 for a real span).   */
    char              *message;  /* Owned, NUL-terminated, already formatted.      */
} qcc_diag;

/*
 * A growable collection of diagnostics plus per-severity tallies. Treat the
 * fields as private; use the functions below.
 */
typedef struct qcc_diag_sink {
    qcc_diag *items;
    size_t    count;
    size_t    capacity;
    size_t    severity_counts[QCC_DIAG_SEVERITY_COUNT];
} qcc_diag_sink;

/* Initialize an empty sink. Always succeeds. */
void qcc_diag_sink_init(qcc_diag_sink *sink);

/* Free all owned messages and the backing array; leaves the sink empty/zeroed. */
void qcc_diag_sink_dispose(qcc_diag_sink *sink);

/*
 * Record a diagnostic. The message is formatted printf-style into a sink-owned
 * buffer. `source` may be NULL for messages not tied to a span (e.g. CLI errors).
 *
 * Returns QCC_OK, or QCC_ERR_OUT_OF_MEMORY if the message or the array could not
 * be allocated. The variadic form forwards to the va_list form.
 */
qcc_status qcc_diag_emit(qcc_diag_sink *sink, qcc_diag_severity severity,
                         const qcc_source *source, size_t offset, size_t length,
                         const char *fmt, ...);

qcc_status qcc_diag_emitv(qcc_diag_sink *sink, qcc_diag_severity severity,
                          const qcc_source *source, size_t offset, size_t length,
                          const char *fmt, va_list args);

/* Total number of diagnostics recorded. */
size_t qcc_diag_count(const qcc_diag_sink *sink);

/* Number of diagnostics of a given severity (e.g. error count to set exit code). */
size_t qcc_diag_severity_count(const qcc_diag_sink *sink, qcc_diag_severity severity);

/* Stable lowercase name of a severity ("error", "warning", "note"). */
const char *qcc_diag_severity_str(qcc_diag_severity severity);

/*
 * Print all diagnostics to `out` in source order of insertion, each as:
 *
 *     name:line:col: severity: message
 *         <the offending source line>
 *         <spaces/tabs>^~~~   (caret under the span)
 *
 * For diagnostics without a source, only the "severity: message" line is printed.
 */
void qcc_diag_sink_print(const qcc_diag_sink *sink, FILE *out);

#endif /* QCC_DIAG_DIAG_H */
