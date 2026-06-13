/*
 * qcc — string interner
 *
 * Responsibility
 * Map each distinct byte span to a single, stable, NUL-terminated copy, so that
 * two spans with identical contents become the *same pointer*. The preprocessor
 * leans on this three ways (ADR-0014): macro lookup and the hide-set membership
 * test reduce to pointer comparisons; `#`/`##` results are interned like any
 * other token so downstream code is uniform; and the §6.10.3 ¶2 "identical
 * redefinition is allowed" check compares replacement lists by interned-pointer
 * sequence rather than by re-lexing.
 *
 * Design (and why)
 *   - Open-addressing hash table (linear probing, power-of-two capacity) keyed
 *     by content. Open addressing keeps the table in one contiguous array — one
 *     malloc, cache-friendly probes — instead of a pointer-chasing chain per
 *     bucket. The hash is 64-bit FNV-1a, the same function qas's symbol table
 *     uses: simple, fast, and well-distributed for short identifiers.
 *   - Interned bytes are copied into a caller-provided arena, so the interner
 *     adds no second lifetime to reason about: every interned string lives
 *     exactly as long as that arena. The interner owns only its bucket array,
 *     which it grows with the seed allocator and frees on dispose.
 *   - Keyed by (bytes, length), not by C-string semantics, so a span containing
 *     an interior NUL still interns correctly; the stored copy is additionally
 *     NUL-terminated for callers that want a C string.
 *
 * Ownership
 *   The arena (borrowed at init) must outlive the interner and every pointer it
 *   returns. qcc_intern_dispose frees only the internal table — never the
 *   interned bytes, which belong to the arena. Returned pointers are const:
 *   interned text is immutable and shared.
 *
 * Standard: ISO/IEC 9899 (C11), portable subset (ADR-0006). Builds on the
 * `arena` module; reports failure via qcc_status / NULL per error-handling.md.
 */
#ifndef QCC_INTERN_INTERN_H
#define QCC_INTERN_INTERN_H

#include <stddef.h>
#include <stdint.h>

#include "arena/arena.h"
#include "status/status.h"

/* One hash-table slot. An empty slot has str == NULL. Defined here (not hidden)
   only so qcc_intern can embed the array by pointer; treat it as private. */
typedef struct qcc_intern_slot {
    const char *str;     /* Interned, NUL-terminated bytes; NULL if slot empty. */
    size_t      length;  /* Content length (excludes the terminating NUL).      */
    uint64_t    hash;    /* Cached FNV-1a of the content (avoids rehash on grow).*/
} qcc_intern_slot;

/*
 * A string interner over one arena. Treat the fields as private; use the
 * functions below. May live on the stack — only `slots` is heap-owned.
 */
typedef struct qcc_intern {
    qcc_arena       *arena;        /* Borrowed; holds the interned byte copies.  */
    qcc_intern_slot *slots;        /* Open-addressing table (power-of-two size). */
    size_t           bucket_count; /* Slot count; always a power of two, or 0.   */
    size_t           count;        /* Number of distinct strings interned.       */
} qcc_intern;

/*
 * Initialize an interner that stores its strings in `arena` (must be non-NULL
 * and outlive the interner). Allocates the initial bucket array. Returns QCC_OK
 * or QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY; on failure *interner is
 * left safe to pass to qcc_intern_dispose.
 */
qcc_status qcc_intern_init(qcc_intern *interner, qcc_arena *arena);

/* Free the bucket array and zero the interner. Does NOT free interned bytes
   (the arena owns those). Idempotent and NULL-safe. */
void qcc_intern_dispose(qcc_intern *interner);

/*
 * Intern a byte span. Returns a stable, NUL-terminated pointer that is byte-for-
 * byte equal to [bytes, bytes+length) and is *the same pointer* for every span
 * with identical contents. `bytes` may be NULL only when length == 0. Returns
 * NULL on out-of-memory (the table is left unchanged and usable). The returned
 * pointer is valid until the backing arena is reset/disposed.
 */
const char *qcc_intern_bytes(qcc_intern *interner, const char *bytes, size_t length);

/* Convenience: intern a NUL-terminated C string (interns strlen(s) bytes). `s`
   must be non-NULL. Returns NULL on OOM. */
const char *qcc_intern_str(qcc_intern *interner, const char *s);

/* Number of distinct strings interned so far. */
size_t qcc_intern_count(const qcc_intern *interner);

#endif /* QCC_INTERN_INTERN_H */
