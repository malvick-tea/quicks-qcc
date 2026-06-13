/*
 * qcc — preprocessor internals: the conditional-inclusion stack (ISO C11 §6.10.1)
 *
 * Responsibility
 * Track the nest of #if / #ifdef / #ifndef … #elif … #else … #endif groups and
 * answer the one question the driver asks on every line: "are we emitting?". A
 * conditional has at most one active group; once a group is taken, no later
 * #elif/#else in the same conditional is taken (§6.10.1 ¶2-6). A conditional
 * nested inside a skipped group is itself entirely skipped — captured by each
 * frame remembering whether its *enclosing* region was emitting.
 *
 * The frame is a plain data record; the branch logic (which group is taken, the
 * #elif/#else transitions) lives in the directive layer that owns conditional
 * semantics. This module just stores the stack and computes "emitting".
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_COND_H
#define QCC_PP_INTERNAL_COND_H

#include <stddef.h>

#include "status/status.h"

/*
 * One conditional in progress. Fields are mutated by the directive layer as
 * #elif/#else are seen.
 *   outer_active : was the enclosing region emitting when this #if was opened?
 *                  If not, the whole conditional is skipped and no controlling
 *                  expression is evaluated.
 *   taken        : has some group of this conditional already been taken? (then
 *                  later #elif/#else are not.)
 *   active       : is the current group emitting? (outer_active && this group's
 *                  condition && !previously-taken)
 *   seen_else    : has #else been seen? (a second #else or any #elif after it is
 *                  an error, §6.10.1 ¶1)
 */
typedef struct qcc_cond {
    int outer_active;
    int taken;
    int active;
    int seen_else;
} qcc_cond;

/* A growable stack of conditionals. Heap-owned `items`; treat as private. */
typedef struct qcc_cond_stack {
    qcc_cond *items;
    size_t    count;
    size_t    capacity;
} qcc_cond_stack;

/* Initialize an empty stack. Always succeeds (lazy allocation on first push). */
void qcc_cond_stack_init(qcc_cond_stack *stack);

/* Free the backing array and zero the stack. NULL-safe, idempotent. */
void qcc_cond_stack_dispose(qcc_cond_stack *stack);

/* Drop all frames but keep the allocation (reset for a new run). */
void qcc_cond_stack_clear(qcc_cond_stack *stack);

/*
 * Are we currently emitting tokens? True when the stack is empty (top level) or
 * the innermost conditional's current group is active. This gates macro
 * expansion, token output, and the execution of non-conditional directives.
 */
int qcc_cond_emitting(const qcc_cond_stack *stack);

/* Number of open conditionals (0 means none; nonzero at EOF is an error). */
size_t qcc_cond_depth(const qcc_cond_stack *stack);

/*
 * Push a new conditional opened by #if/#ifdef/#ifndef. `outer_active` is the
 * emitting state just before it (qcc_cond_emitting), and `condition` is the
 * controlling result (evaluated by the caller only when outer_active). The new
 * frame's active group is outer_active && condition. Returns QCC_OK or
 * QCC_ERR_OUT_OF_MEMORY.
 */
qcc_status qcc_cond_push(qcc_cond_stack *stack, int outer_active, int condition);

/* The innermost open conditional, or NULL if none. The caller mutates its
   fields to implement #elif/#else. */
qcc_cond *qcc_cond_top(qcc_cond_stack *stack);

/* Pop the innermost conditional (for #endif). Returns 1 if one was popped, 0 if
   the stack was already empty (#endif without #if). */
int qcc_cond_pop(qcc_cond_stack *stack);

#endif /* QCC_PP_INTERNAL_COND_H */
