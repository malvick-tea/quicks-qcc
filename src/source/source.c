/*
 * qcc — source buffers and source locations: implementation.
 *
 * See source.h for the contract and the rationale behind the NUL sentinel and
 * the precomputed line-start index. Standard library usage (stdio/stdlib/string)
 * is a permitted seed dependency for host tools (ADR-0009).
 */
#include "source/source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small private helpers (file-local; no module prefix needed). */

/*
 * Duplicate a NUL-terminated string with malloc. We provide our own instead of
 * POSIX strdup() because ADR-0009 restricts host tools to portable ISO C; strdup
 * is POSIX, not ISO C. Returns NULL on allocation failure.
 */
static char *dup_cstr(const char *s)
{
    size_t len = strlen(s);
    char  *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1); /* +1 copies the terminating NUL as well. */
    return copy;
}

/*
 * Build the line-start index for src->data[0 .. src->size). Must be called after
 * data/size are set. Allocates src->line_starts and sets src->line_count.
 *
 * Line 1 begins at offset 0. Every byte immediately following a '\n' begins a
 * new line. Therefore line_count == (number of '\n') + 1, and an input that ends
 * with '\n' has a final (possibly empty) line — which is the conventional model
 * used by editors and compilers.
 */
static qcc_status build_line_starts(qcc_source *src)
{
    size_t newline_count = 0;
    for (size_t i = 0; i < src->size; ++i) {
        if (src->data[i] == '\n') {
            ++newline_count;
        }
    }

    size_t count = newline_count + 1;
    size_t *starts = (size_t *)malloc(count * sizeof(*starts));
    if (starts == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    starts[0] = 0;
    size_t next = 1;
    for (size_t i = 0; i < src->size && next < count; ++i) {
        if (src->data[i] == '\n') {
            starts[next++] = i + 1; /* The line after this newline starts here. */
        }
    }

    src->line_starts = starts;
    src->line_count  = count;
    return QCC_OK;
}

/* Public interface. */

qcc_status qcc_source_from_memory(const char *name, const char *bytes,
                                  size_t size, qcc_source *out)
{
    if (out == NULL || (bytes == NULL && size != 0)) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_source src;
    src.name        = NULL;
    src.data        = NULL;
    src.size        = size;
    src.line_starts = NULL;
    src.line_count  = 0;

    /* Copy the display name (defaulting to a placeholder for anonymous input). */
    src.name = dup_cstr(name != NULL ? name : "<memory>");
    if (src.name == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    /* Copy the content with one extra byte for the guaranteed NUL sentinel. */
    src.data = (char *)malloc(size + 1);
    if (src.data == NULL) {
        free(src.name);
        return QCC_ERR_OUT_OF_MEMORY;
    }
    if (size != 0) {
        memcpy(src.data, bytes, size);
    }
    src.data[size] = '\0';

    qcc_status st = build_line_starts(&src);
    if (st != QCC_OK) {
        free(src.data);
        free(src.name);
        return st;
    }

    *out = src;
    return QCC_OK;
}

qcc_status qcc_source_load_file(const char *path, qcc_source *out)
{
    if (path == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    /* Binary mode: we must see the exact bytes (no CRLF translation by the CRT),
       because the compiler reasons about precise byte offsets and line endings. */
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return QCC_ERR_IO;
    }

    /* Declared before any `goto done` so no initialization is jumped over. */
    qcc_status st     = QCC_ERR_IO;
    char      *buffer = NULL;
    long       tell   = 0;
    size_t     size   = 0;
    size_t     got    = 0;

    if (fseek(fp, 0L, SEEK_END) != 0) {
        goto done;
    }

    tell = ftell(fp);
    if (tell < 0) {
        goto done;
    }
    rewind(fp);

    size = (size_t)tell;
    buffer = (char *)malloc(size + 1); /* +1 for the NUL sentinel. */
    if (buffer == NULL) {
        st = QCC_ERR_OUT_OF_MEMORY;
        goto done;
    }

    got = fread(buffer, 1, size, fp);
    if (got != size) {
        /* A short read on a regular file opened read-only is an I/O failure. */
        st = QCC_ERR_IO;
        goto done;
    }
    buffer[size] = '\0';

    /* Hand the already-read bytes to from_memory, which copies them and builds
       the line index. The extra copy is negligible and keeps a single code path
       for buffer/line-table setup. */
    st = qcc_source_from_memory(path, buffer, size, out);

done:
    free(buffer);
    fclose(fp);
    return st;
}

void qcc_source_dispose(qcc_source *src)
{
    if (src == NULL) {
        return;
    }
    free(src->line_starts);
    free(src->data);
    free(src->name);
    src->name        = NULL;
    src->data        = NULL;
    src->size        = 0;
    src->line_starts = NULL;
    src->line_count  = 0;
}

qcc_status qcc_source_location(const qcc_source *src, size_t offset,
                               uint32_t *out_line, uint32_t *out_column)
{
    if (src == NULL || out_line == NULL || out_column == NULL ||
        src->line_starts == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    /* Clamp past-the-end offsets to end-of-input so an EOF token is locatable. */
    if (offset > src->size) {
        offset = src->size;
    }

    /* upper_bound: smallest index `lo` such that line_starts[lo] > offset.
       Because line_starts[0] == 0 <= offset, lo is always >= 1, and (lo-1) is the
       0-based index of the line containing `offset`. */
    size_t lo = 0;
    size_t hi = src->line_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (src->line_starts[mid] <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    *out_line   = (uint32_t)lo;                                  /* 1-based line.   */
    *out_column = (uint32_t)(offset - src->line_starts[lo - 1] + 1); /* 1-based col. */
    return QCC_OK;
}

const char *qcc_source_line_text(const qcc_source *src, uint32_t line,
                                 size_t *out_length)
{
    if (src == NULL || out_length == NULL || line < 1 ||
        (size_t)line > src->line_count) {
        return NULL;
    }

    size_t start = src->line_starts[line - 1];
    size_t end   = ((size_t)line < src->line_count)
                       ? src->line_starts[line] /* start of the next line.      */
                       : src->size;             /* last line runs to end-of-input. */

    /* Trim the line terminator so callers get just the visible text. Handles both
       "\n" and "\r\n" (and a stray trailing "\r"). */
    size_t length = end - start;
    while (length > 0 &&
           (src->data[start + length - 1] == '\n' ||
            src->data[start + length - 1] == '\r')) {
        --length;
    }

    *out_length = length;
    return src->data + start;
}
