/*
 * qcc — string interner: implementation.
 *
 * See intern.h for the contract and rationale. The table is open-addressing
 * with linear probing and a power-of-two capacity, so a bucket index is a cheap
 * `hash & (bucket_count - 1)` mask. Content copies live in the borrowed arena;
 * the table itself uses the seed allocator and grows when the load factor would
 * exceed the threshold below, rehashing from the cached per-slot hash.
 */
#include "intern/intern.h"

#include <stdlib.h>
#include <string.h>

/*
 * Initial number of buckets (a power of two). Small enough that interning a
 * handful of identifiers does not reserve a large table, large enough to avoid
 * an immediate grow for typical inputs.
 */
#define QCC_INTERN_INITIAL_BUCKETS ((size_t)64u)

/*
 * Grow when count * GROW_NUM >= bucket_count * GROW_DEN, i.e. at load factor
 * 3/4. Linear probing degrades sharply past ~0.7-0.8, so 0.75 keeps probe
 * chains short while not wasting much memory.
 */
#define QCC_INTERN_GROW_NUM ((size_t)4u)
#define QCC_INTERN_GROW_DEN ((size_t)3u)

/* FNV-1a 64-bit constants (the canonical offset basis and prime). The same hash
   qas's symbol table uses; good distribution for short identifier-like keys. */
#define QCC_FNV1A_OFFSET_BASIS ((uint64_t)1469598103934665603u)
#define QCC_FNV1A_PRIME        ((uint64_t)1099511628211u)

/* Private helpers. */

static uint64_t fnv1a(const char *bytes, size_t length)
{
    uint64_t hash = QCC_FNV1A_OFFSET_BASIS;
    for (size_t i = 0; i < length; ++i) {
        hash ^= (uint64_t)(unsigned char)bytes[i];
        hash *= QCC_FNV1A_PRIME;
    }
    return hash;
}

/*
 * Find the slot for (bytes, length, hash) in `slots` (capacity buckets). Returns
 * the index of the matching slot if present, or of the first empty slot where it
 * would be inserted. `buckets` must be a power of two and the table must have at
 * least one empty slot (the load factor guarantees this), so the probe always
 * terminates.
 */
static size_t probe(const qcc_intern_slot *slots, size_t buckets,
                     const char *bytes, size_t length, uint64_t hash)
{
    size_t mask = buckets - 1u;
    size_t i    = (size_t)hash & mask;
    for (;;) {
        const qcc_intern_slot *slot = &slots[i];
        if (slot->str == NULL) {
            return i; /* Empty: insertion point. */
        }
        if (slot->hash == hash && slot->length == length &&
            memcmp(slot->str, bytes, length) == 0) {
            return i; /* Match. */
        }
        i = (i + 1u) & mask; /* Linear probe, wrapping. */
    }
}

/* Allocate a zeroed bucket array of `buckets` slots, or NULL on OOM. */
static qcc_intern_slot *alloc_slots(size_t buckets)
{
    return (qcc_intern_slot *)calloc(buckets, sizeof(qcc_intern_slot));
}

/*
 * Double the table and re-insert existing entries. Returns QCC_OK, or
 * QCC_ERR_OUT_OF_MEMORY leaving the current table intact (so the caller can
 * still serve from it / report the failure).
 */
static qcc_status grow(qcc_intern *interner)
{
    size_t new_buckets = interner->bucket_count * 2u;
    if (new_buckets < interner->bucket_count) {
        return QCC_ERR_OUT_OF_MEMORY; /* Size overflow: cannot grow further. */
    }
    qcc_intern_slot *new_slots = alloc_slots(new_buckets);
    if (new_slots == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < interner->bucket_count; ++i) {
        const qcc_intern_slot *old = &interner->slots[i];
        if (old->str == NULL) {
            continue;
        }
        size_t j = probe(new_slots, new_buckets, old->str, old->length, old->hash);
        new_slots[j] = *old; /* Reuse the cached hash; no rehash of bytes. */
    }

    free(interner->slots);
    interner->slots        = new_slots;
    interner->bucket_count = new_buckets;
    return QCC_OK;
}

/* Public interface. */

qcc_status qcc_intern_init(qcc_intern *interner, qcc_arena *arena)
{
    if (interner == NULL || arena == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    interner->arena        = arena;
    interner->slots        = NULL;
    interner->bucket_count = 0;
    interner->count        = 0;

    qcc_intern_slot *slots = alloc_slots(QCC_INTERN_INITIAL_BUCKETS);
    if (slots == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    interner->slots        = slots;
    interner->bucket_count = QCC_INTERN_INITIAL_BUCKETS;
    return QCC_OK;
}

void qcc_intern_dispose(qcc_intern *interner)
{
    if (interner == NULL) {
        return;
    }
    free(interner->slots);
    interner->arena        = NULL;
    interner->slots        = NULL;
    interner->bucket_count = 0;
    interner->count        = 0;
}

const char *qcc_intern_bytes(qcc_intern *interner, const char *bytes, size_t length)
{
    if (interner == NULL || interner->slots == NULL ||
        (bytes == NULL && length != 0)) {
        return NULL;
    }

    uint64_t hash = fnv1a(bytes, length);
    size_t   i    = probe(interner->slots, interner->bucket_count, bytes, length,
                          hash);
    if (interner->slots[i].str != NULL) {
        return interner->slots[i].str; /* Already interned: same pointer. */
    }

    /* Grow before inserting if this insertion would exceed the load factor, so
       the table always keeps an empty slot (probe-termination invariant). */
    if ((interner->count + 1u) * QCC_INTERN_GROW_NUM >=
        interner->bucket_count * QCC_INTERN_GROW_DEN) {
        qcc_status st = grow(interner);
        if (st != QCC_OK) {
            return NULL;
        }
        i = probe(interner->slots, interner->bucket_count, bytes, length, hash);
    }

    /* Copy the content into the arena and NUL-terminate it. The terminator is a
       convenience for C-string callers; comparisons use the stored length. */
    char *copy = (char *)qcc_arena_alloc(interner->arena, length + 1u, 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0) {
        memcpy(copy, bytes, length);
    }
    copy[length] = '\0';

    interner->slots[i].str    = copy;
    interner->slots[i].length = length;
    interner->slots[i].hash   = hash;
    interner->count          += 1;
    return copy;
}

const char *qcc_intern_str(qcc_intern *interner, const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return qcc_intern_bytes(interner, s, strlen(s));
}

size_t qcc_intern_count(const qcc_intern *interner)
{
    return (interner != NULL) ? interner->count : 0;
}
