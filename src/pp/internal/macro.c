/*
 * qcc — preprocessor internals: macro table (implementation).
 *
 * See macro.h for the contract. The table is open-addressing with linear
 * probing and a power-of-two capacity, keyed by interned name pointer; the
 * bucket index is `mix(pointer) & (bucket_count - 1)`. Records live in the
 * arena; the table owns only its bucket array. Deletion uses backward-shift so
 * the probe-chain invariant holds without tombstones.
 */
#include "pp/internal/macro.h"

#include <stdint.h>
#include <stdlib.h>

#define QCC_MACRO_INITIAL_BUCKETS ((size_t)64u)
#define QCC_MACRO_GROW_NUM        ((size_t)4u) /* grow at load factor 3/4 */
#define QCC_MACRO_GROW_DEN        ((size_t)3u)

/* Private helpers. */

/*
 * Hash an interned name *pointer*. The MurmurHash3 64-bit finalizer scrambles
 * the pointer's low-bit regularities (allocations are typically 8/16-aligned,
 * so the low bits are poor hash material) into a well-distributed value.
 */
static uint64_t mix_pointer(const char *name)
{
    uint64_t h = (uint64_t)(uintptr_t)name;
    h ^= h >> 33;
    h *= (uint64_t)0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= (uint64_t)0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static qcc_macro_slot *alloc_slots(size_t buckets)
{
    return (qcc_macro_slot *)calloc(buckets, sizeof(qcc_macro_slot));
}

/* Index of `name`'s slot (match) or the first empty slot for it. The load
   factor guarantees an empty slot exists, so the probe terminates. */
static size_t probe(const qcc_macro_slot *slots, size_t buckets, const char *name)
{
    size_t mask = buckets - 1u;
    size_t i    = (size_t)mix_pointer(name) & mask;
    while (slots[i].key != NULL && slots[i].key != name) {
        i = (i + 1u) & mask;
    }
    return i;
}

static qcc_status grow(qcc_macro_table *table)
{
    size_t new_buckets = table->bucket_count * 2u;
    if (new_buckets < table->bucket_count) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    qcc_macro_slot *new_slots = alloc_slots(new_buckets);
    if (new_slots == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < table->bucket_count; ++i) {
        if (table->slots[i].key != NULL) {
            size_t j = probe(new_slots, new_buckets, table->slots[i].key);
            new_slots[j] = table->slots[i];
        }
    }
    free(table->slots);
    table->slots        = new_slots;
    table->bucket_count = new_buckets;
    return QCC_OK;
}

/* Public interface. */

qcc_status qcc_macro_table_init(qcc_macro_table *table)
{
    if (table == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    table->slots        = alloc_slots(QCC_MACRO_INITIAL_BUCKETS);
    table->bucket_count = QCC_MACRO_INITIAL_BUCKETS;
    table->count        = 0;
    if (table->slots == NULL) {
        table->bucket_count = 0;
        return QCC_ERR_OUT_OF_MEMORY;
    }
    return QCC_OK;
}

void qcc_macro_table_dispose(qcc_macro_table *table)
{
    if (table == NULL) {
        return;
    }
    free(table->slots);
    table->slots        = NULL;
    table->bucket_count = 0;
    table->count        = 0;
}

qcc_macro *qcc_macro_lookup(const qcc_macro_table *table, const char *name)
{
    if (table == NULL || table->slots == NULL || name == NULL) {
        return NULL;
    }
    size_t i = probe(table->slots, table->bucket_count, name);
    return (table->slots[i].key != NULL) ? table->slots[i].macro : NULL;
}

qcc_status qcc_macro_put(qcc_macro_table *table, qcc_macro *macro)
{
    if (table == NULL || table->slots == NULL || macro == NULL ||
        macro->name == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    size_t i = probe(table->slots, table->bucket_count, macro->name);
    if (table->slots[i].key != NULL) {
        table->slots[i].macro = macro; /* Redefine in place: same key slot. */
        return QCC_OK;
    }

    if ((table->count + 1u) * QCC_MACRO_GROW_NUM >=
        table->bucket_count * QCC_MACRO_GROW_DEN) {
        qcc_status st = grow(table);
        if (st != QCC_OK) {
            return st;
        }
        i = probe(table->slots, table->bucket_count, macro->name);
    }
    table->slots[i].key   = macro->name;
    table->slots[i].macro = macro;
    table->count         += 1;
    return QCC_OK;
}

int qcc_macro_remove(qcc_macro_table *table, const char *name)
{
    if (table == NULL || table->slots == NULL || name == NULL) {
        return 0;
    }
    size_t mask = table->bucket_count - 1u;
    size_t i    = probe(table->slots, table->bucket_count, name);
    if (table->slots[i].key == NULL) {
        return 0; /* Not present. */
    }

    /*
     * Backward-shift deletion (Knuth, "Algorithm R"): empty the slot, then walk
     * forward pulling back any entry whose ideal bucket lies outside the open-
     * closed interval (hole, j], because such an entry's probe chain passes
     * through the hole and must fill it to remain findable.
     */
    for (;;) {
        table->slots[i].key   = NULL;
        table->slots[i].macro = NULL;
        size_t j = i;
        for (;;) {
            j = (j + 1u) & mask;
            if (table->slots[j].key == NULL) {
                table->count -= 1;
                return 1;
            }
            size_t home  = (size_t)mix_pointer(table->slots[j].key) & mask;
            size_t dist_home = (home - i) & mask; /* steps from hole to home   */
            size_t dist_j    = (j - i) & mask;    /* steps from hole to j      */
            /* Keep scanning while home is in (i, j]; otherwise move j into i. */
            if (!(dist_home >= 1u && dist_home <= dist_j)) {
                break;
            }
        }
        table->slots[i] = table->slots[j];
        i = j;
    }
}

size_t qcc_macro_count(const qcc_macro_table *table)
{
    return (table != NULL) ? table->count : 0;
}

int qcc_macro_identical(const qcc_macro *a, const qcc_macro *b)
{
    if (a == NULL || b == NULL) {
        return a == b; /* Both NULL -> identical; one NULL -> not. */
    }
    if (a->is_function_like != b->is_function_like ||
        a->is_variadic != b->is_variadic ||
        a->param_count != b->param_count ||
        a->replacement_count != b->replacement_count) {
        return 0;
    }
    for (size_t i = 0; i < a->param_count; ++i) {
        if (a->params[i] != b->params[i]) { /* Interned: pointer identity. */
            return 0;
        }
    }
    for (size_t i = 0; i < a->replacement_count; ++i) {
        const qcc_ptok *ta = &a->replacement[i];
        const qcc_ptok *tb = &b->replacement[i];
        if (ta->kind != tb->kind || ta->spelling != tb->spelling) {
            return 0;
        }
        if (ta->kind == QCC_PP_TOKEN_PUNCT && ta->punct != tb->punct) {
            return 0;
        }
        /* "all white-space separations are considered identical" (§6.10.3 ¶1):
           compare only the presence of preceding whitespace, not its amount. */
        if ((ta->leading_space != 0) != (tb->leading_space != 0)) {
            return 0;
        }
    }
    return 1;
}
