/*
 * qcc — preprocessor internals: hide sets (ISO C11 §6.10.3.4)
 *
 * Responsibility
 * Represent, for one preprocessing token, the set of macro names that must NOT
 * be expanded into it again. This is the mechanism behind §6.10.3.4 ¶2: "If the
 * name of the macro being replaced is found during this scan of the replacement
 * list … it is not replaced. Furthermore, if any nested replacements encounter
 * the name of the macro being replaced, it is not replaced." A token is eligible
 * for macro expansion only if its spelling names a defined macro AND that name
 * is not in the token's hide set.
 *
 * This is the Prosser formulation (the published model that the standard's prose
 * describes operationally): object-like expansion adds the macro name to every
 * replacement token's hide set; function-like expansion adds the *intersection*
 * of the name token's and the closing ')'s hide sets, then the macro name. Doing
 * it this way — rather than a recursion-depth counter or a single "in progress"
 * flag — is the only representation that gets the standard's self- and mutual-
 * recursion corner cases right (ADR-0014).
 *
 * Representation (and why)
 *   An immutable singly linked list of interned macro-name pointers, allocated
 *   in the preprocessor's arena. Names are interned (the `intern` module), so
 *   membership is pointer comparison, not strcmp. Immutability lets sets share
 *   tails freely: adding a name conses one cell onto an existing set without
 *   copying it, and the old set stays valid for the tokens that still reference
 *   it. The empty set is the NULL pointer. Sets are small (bounded by the number
 *   of macros active at a point), so the O(n) membership / O(n*m) set algebra is
 *   not a concern.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_HIDESET_H
#define QCC_PP_INTERNAL_HIDESET_H

#include <stddef.h>

#include "arena/arena.h"
#include "status/status.h"

/*
 * One hide-set node: an interned macro name and the rest of the set. The empty
 * set is represented by a NULL qcc_hideset pointer (so there is no separate
 * "empty" object to allocate). Immutable once built.
 */
typedef struct qcc_hideset {
    const char         *name;  /* Interned macro-name pointer (identity compare).*/
    const struct qcc_hideset *next;
} qcc_hideset;

/*
 * Membership test. `name` must be an interned pointer (the same pointer the set
 * holds), since comparison is by pointer identity. Returns 1 if present, 0 if
 * absent or `set` is the empty (NULL) set.
 */
int qcc_hideset_contains(const qcc_hideset *set, const char *name);

/* Number of names in the set (0 for the empty set). */
size_t qcc_hideset_size(const qcc_hideset *set);

/*
 * Add an interned `name` to `set`, returning the new set via *out. If `name` is
 * already present, *out == set and nothing is allocated. Otherwise one cell is
 * consed in `arena`. Returns QCC_OK, QCC_ERR_INVALID_ARGUMENT (NULL arena/out/
 * name), or QCC_ERR_OUT_OF_MEMORY.
 */
qcc_status qcc_hideset_add(qcc_arena *arena, const qcc_hideset *set,
                           const char *name, const qcc_hideset **out);

/*
 * Set union: every name in `a` or `b`, no duplicates. Result via *out (may be
 * the NULL empty set). Returns QCC_OK or an allocation/argument error. The
 * inputs are unchanged (immutable).
 */
qcc_status qcc_hideset_union(qcc_arena *arena, const qcc_hideset *a,
                             const qcc_hideset *b, const qcc_hideset **out);

/*
 * Set intersection: every name in both `a` and `b`. Result via *out (may be the
 * NULL empty set). Returns QCC_OK or an allocation/argument error. Used by
 * function-like macro expansion (§6.10.3.4): the replacement tokens' hide set
 * starts from HS(name-token) ∩ HS(')').
 */
qcc_status qcc_hideset_intersect(qcc_arena *arena, const qcc_hideset *a,
                                 const qcc_hideset *b, const qcc_hideset **out);

#endif /* QCC_PP_INTERNAL_HIDESET_H */
