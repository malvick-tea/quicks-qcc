/*
 * qcc — preprocessor internals: hide sets (implementation).
 *
 * See hideset.h for the contract and the §6.10.3.4 rationale. Sets are
 * immutable cons lists of interned name pointers in the arena; the empty set is
 * NULL. add() conses one cell unless the name is already present; union and
 * intersect are folds built on add() and contains(), so de-duplication lives in
 * exactly one place.
 */
#include "pp/internal/hideset.h"

int qcc_hideset_contains(const qcc_hideset *set, const char *name)
{
    for (const qcc_hideset *node = set; node != NULL; node = node->next) {
        if (node->name == name) { /* Interned: pointer identity is content equality. */
            return 1;
        }
    }
    return 0;
}

size_t qcc_hideset_size(const qcc_hideset *set)
{
    size_t n = 0;
    for (const qcc_hideset *node = set; node != NULL; node = node->next) {
        n += 1;
    }
    return n;
}

qcc_status qcc_hideset_add(qcc_arena *arena, const qcc_hideset *set,
                           const char *name, const qcc_hideset **out)
{
    if (arena == NULL || name == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (qcc_hideset_contains(set, name)) {
        *out = set; /* Already a member: sets are de-duplicated, reuse as-is. */
        return QCC_OK;
    }

    qcc_hideset *node =
        (qcc_hideset *)qcc_arena_alloc(arena, sizeof(*node), _Alignof(qcc_hideset));
    if (node == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    node->name = name;
    node->next = set; /* Share the existing tail (immutable). */
    *out       = node;
    return QCC_OK;
}

qcc_status qcc_hideset_union(qcc_arena *arena, const qcc_hideset *a,
                             const qcc_hideset *b, const qcc_hideset **out)
{
    if (arena == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    const qcc_hideset *result = a;
    for (const qcc_hideset *node = b; node != NULL; node = node->next) {
        qcc_status st = qcc_hideset_add(arena, result, node->name, &result);
        if (st != QCC_OK) {
            return st;
        }
    }
    *out = result;
    return QCC_OK;
}

qcc_status qcc_hideset_intersect(qcc_arena *arena, const qcc_hideset *a,
                                 const qcc_hideset *b, const qcc_hideset **out)
{
    if (arena == NULL || out == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    const qcc_hideset *result = NULL; /* Empty set. */
    for (const qcc_hideset *node = a; node != NULL; node = node->next) {
        if (qcc_hideset_contains(b, node->name)) {
            qcc_status st = qcc_hideset_add(arena, result, node->name, &result);
            if (st != QCC_OK) {
                return st;
            }
        }
    }
    *out = result;
    return QCC_OK;
}
