/*
 * qcc — diagnostics: implementation.
 *
 * See diag.h for the contract. The message is formatted with vsnprintf in the
 * standard two-pass idiom: once to measure the required length, once to fill an
 * exactly-sized buffer. va_copy is required because a va_list may be consumed by
 * the first vsnprintf and must not be reused without copying (C11 §7.16).
 */
#include "diag/diag.h"

#include <stdlib.h>
#include <string.h>

/* Private helpers. */

/*
 * Ensure the sink can hold at least one more diagnostic, growing geometrically.
 * Geometric growth keeps total appends amortized O(1). Returns QCC_OK or
 * QCC_ERR_OUT_OF_MEMORY (leaving the existing buffer intact on failure).
 */
static qcc_status sink_reserve_one(qcc_diag_sink *sink)
{
    if (sink->count < sink->capacity) {
        return QCC_OK;
    }

    size_t new_capacity = (sink->capacity == 0) ? 8u : sink->capacity * 2u;
    qcc_diag *grown =
        (qcc_diag *)realloc(sink->items, new_capacity * sizeof(*grown));
    if (grown == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    sink->items    = grown;
    sink->capacity = new_capacity;
    return QCC_OK;
}

/*
 * Format `fmt`/`args` into a freshly malloc'd, NUL-terminated string.
 * Returns NULL on allocation failure or on an encoding error from vsnprintf.
 */
static char *format_message(const char *fmt, va_list args)
{
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);

    if (needed < 0) {
        return NULL; /* Output/encoding error from the formatter. */
    }

    size_t size = (size_t)needed + 1; /* +1 for the terminating NUL. */
    char  *buffer = (char *)malloc(size);
    if (buffer == NULL) {
        return NULL;
    }

    (void)vsnprintf(buffer, size, fmt, args);
    return buffer;
}

/* Public interface. */

void qcc_diag_sink_init(qcc_diag_sink *sink)
{
    if (sink == NULL) {
        return;
    }
    sink->items    = NULL;
    sink->count    = 0;
    sink->capacity = 0;
    for (int i = 0; i < QCC_DIAG_SEVERITY_COUNT; ++i) {
        sink->severity_counts[i] = 0;
    }
}

void qcc_diag_sink_dispose(qcc_diag_sink *sink)
{
    if (sink == NULL) {
        return;
    }
    for (size_t i = 0; i < sink->count; ++i) {
        free(sink->items[i].message);
    }
    free(sink->items);
    qcc_diag_sink_init(sink); /* Reset to a clean, reusable empty state. */
}

qcc_status qcc_diag_emitv(qcc_diag_sink *sink, qcc_diag_severity severity,
                          const qcc_source *source, size_t offset, size_t length,
                          const char *fmt, va_list args)
{
    if (sink == NULL || fmt == NULL ||
        (int)severity < 0 || severity >= QCC_DIAG_SEVERITY_COUNT) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_status st = sink_reserve_one(sink);
    if (st != QCC_OK) {
        return st;
    }

    char *message = format_message(fmt, args);
    if (message == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    qcc_diag *slot = &sink->items[sink->count];
    slot->severity = severity;
    slot->source   = source;
    slot->offset   = offset;
    slot->length   = length;
    slot->message  = message;

    sink->count += 1;
    sink->severity_counts[severity] += 1;
    return QCC_OK;
}

qcc_status qcc_diag_emit(qcc_diag_sink *sink, qcc_diag_severity severity,
                         const qcc_source *source, size_t offset, size_t length,
                         const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    qcc_status st =
        qcc_diag_emitv(sink, severity, source, offset, length, fmt, args);
    va_end(args);
    return st;
}

size_t qcc_diag_count(const qcc_diag_sink *sink)
{
    return (sink != NULL) ? sink->count : 0;
}

size_t qcc_diag_severity_count(const qcc_diag_sink *sink, qcc_diag_severity severity)
{
    if (sink == NULL || (int)severity < 0 || severity >= QCC_DIAG_SEVERITY_COUNT) {
        return 0;
    }
    return sink->severity_counts[severity];
}

const char *qcc_diag_severity_str(qcc_diag_severity severity)
{
    switch (severity) {
    case QCC_DIAG_NOTE:           return "note";
    case QCC_DIAG_WARNING:        return "warning";
    case QCC_DIAG_ERROR:          return "error";
    case QCC_DIAG_SEVERITY_COUNT: break; /* Not a real severity; fall through. */
    }
    return "diagnostic";
}

/*
 * Print one diagnostic's caret line: leading indentation that mirrors the source
 * (tabs preserved so the caret aligns under proportional/tabbed text), a '^' at
 * the span start, and '~' for the remainder of the span, clipped to the line.
 */
static void print_caret(FILE *out, const char *line_text, size_t line_length,
                        uint32_t column, size_t span_length)
{
    fputs("    ", out); /* Match the 4-space indent used for the source line. */

    size_t caret_col = (column >= 1) ? (size_t)(column - 1) : 0; /* 0-based. */
    for (size_t i = 0; i < caret_col && i < line_length; ++i) {
        fputc(line_text[i] == '\t' ? '\t' : ' ', out);
    }

    fputc('^', out);

    /* Underline the rest of the span (if any), but never run past the line. */
    size_t remaining = (span_length > 1) ? (span_length - 1) : 0;
    size_t available =
        (caret_col + 1 < line_length) ? (line_length - (caret_col + 1)) : 0;
    if (remaining > available) {
        remaining = available;
    }
    for (size_t i = 0; i < remaining; ++i) {
        fputc('~', out);
    }
    fputc('\n', out);
}

void qcc_diag_sink_print(const qcc_diag_sink *sink, FILE *out)
{
    if (sink == NULL || out == NULL) {
        return;
    }

    for (size_t i = 0; i < sink->count; ++i) {
        const qcc_diag *d = &sink->items[i];
        const char     *sev = qcc_diag_severity_str(d->severity);

        if (d->source == NULL) {
            /* No source span: just "severity: message". */
            fprintf(out, "%s: %s\n", sev, d->message);
            continue;
        }

        uint32_t line = 0;
        uint32_t col  = 0;
        (void)qcc_source_location(d->source, d->offset, &line, &col);
        fprintf(out, "%s:%u:%u: %s: %s\n", d->source->name, (unsigned)line,
                (unsigned)col, sev, d->message);

        size_t      line_len  = 0;
        const char *line_text = qcc_source_line_text(d->source, line, &line_len);
        if (line_text != NULL) {
            fprintf(out, "    %.*s\n", (int)line_len, line_text);
            print_caret(out, line_text, line_len, col, d->length);
        }
    }
}
