/*
 * qcc — preprocessor internals: the conditional-inclusion stack (implementation).
 *
 * See cond.h. A simple geometric-growth stack of frames; "emitting" reads the
 * top frame's active flag. The #elif/#else transition logic lives in the
 * directive layer, which mutates the top frame returned by qcc_cond_top.
 */
#include "pp/internal/cond.h"

#include <stdlib.h>

void qcc_cond_stack_init(qcc_cond_stack *stack)
{
    if (stack == NULL) {
        return;
    }
    stack->items    = NULL;
    stack->count    = 0;
    stack->capacity = 0;
}

void qcc_cond_stack_dispose(qcc_cond_stack *stack)
{
    if (stack == NULL) {
        return;
    }
    free(stack->items);
    qcc_cond_stack_init(stack);
}

void qcc_cond_stack_clear(qcc_cond_stack *stack)
{
    if (stack != NULL) {
        stack->count = 0;
    }
}

int qcc_cond_emitting(const qcc_cond_stack *stack)
{
    if (stack == NULL || stack->count == 0) {
        return 1; /* Top level: always emitting. */
    }
    return stack->items[stack->count - 1].active;
}

size_t qcc_cond_depth(const qcc_cond_stack *stack)
{
    return (stack != NULL) ? stack->count : 0;
}

qcc_status qcc_cond_push(qcc_cond_stack *stack, int outer_active, int condition)
{
    if (stack == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (stack->count == stack->capacity) {
        size_t cap = (stack->capacity == 0) ? 8u : stack->capacity * 2u;
        qcc_cond *grown = (qcc_cond *)realloc(stack->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        stack->items    = grown;
        stack->capacity = cap;
    }

    qcc_cond *frame      = &stack->items[stack->count++];
    frame->outer_active  = outer_active ? 1 : 0;
    frame->active        = (outer_active && condition) ? 1 : 0;
    frame->taken         = frame->active;
    frame->seen_else     = 0;
    return QCC_OK;
}

qcc_cond *qcc_cond_top(qcc_cond_stack *stack)
{
    if (stack == NULL || stack->count == 0) {
        return NULL;
    }
    return &stack->items[stack->count - 1];
}

int qcc_cond_pop(qcc_cond_stack *stack)
{
    if (stack == NULL || stack->count == 0) {
        return 0;
    }
    stack->count -= 1;
    return 1;
}
