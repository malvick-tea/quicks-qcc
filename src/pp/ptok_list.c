/*
 * qcc — preprocessor: the qcc_ptok growable list (realizes part of pp.h).
 *
 * A plain geometric-growth dynamic array of materialized tokens, separated from
 * the driver (pp.c) so each translation unit keeps one responsibility
 * (coding-standard.md §2). The item array is heap-owned; token spellings are
 * not owned here (they live in the producing preprocessor's interner), so
 * disposing a list never touches spelling memory.
 */
#include "pp/pp.h"

#include <stdlib.h>

void qcc_ptok_list_init(qcc_ptok_list *list)
{
    if (list == NULL) {
        return;
    }
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void qcc_ptok_list_dispose(qcc_ptok_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    qcc_ptok_list_init(list);
}

void qcc_ptok_list_clear(qcc_ptok_list *list)
{
    if (list != NULL) {
        list->count = 0; /* Keep capacity for reuse; spellings are not owned. */
    }
}

qcc_status qcc_ptok_list_push(qcc_ptok_list *list, const qcc_ptok *tok)
{
    if (list == NULL || tok == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (list->count == list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? 16u : list->capacity * 2u;
        qcc_ptok *grown =
            (qcc_ptok *)realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        list->items    = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count] = *tok;
    list->count += 1;
    return QCC_OK;
}
