/*
 * qcc — preprocessor internals: macro records and the macro table (ISO C11 §6.10.3)
 *
 * Responsibility
 * Store the macro definitions in force at a point in the translation unit and
 * answer "is this identifier a macro, and what does it expand to?". A macro is
 * either object-like (`#define NAME replacement`) or function-like
 * (`#define NAME(params) replacement`), possibly variadic (`...`, §6.10.3 ¶12).
 * Replacement lists are stored as already-materialized qcc_ptok sequences in the
 * preprocessor's arena, so expansion (pp/internal/expand) never re-lexes them.
 *
 * Why interned names as keys
 *   Macro names are interned (the `intern` module), so the table is keyed by
 *   pointer identity: lookup hashes and compares pointers, not strings. The same
 *   interned pointer is what a token carries as its spelling, so checking "is
 *   this token a macro" is one hash probe with pointer compares.
 *
 * Identical-redefinition rule (§6.10.3 ¶1-2)
 *   A macro may be redefined only by an identical definition: same form,
 *   same parameters, and replacement lists that match token-for-token with the
 *   same white-space separation (all white-space separations counting as
 *   identical). qcc_macro_identical implements exactly that test; the policy of
 *   what to do on a non-identical redefinition (diagnose) lives in the directive
 *   layer, keeping this table a pure data structure.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_MACRO_H
#define QCC_PP_INTERNAL_MACRO_H

#include <stddef.h>

#include "pp/pp.h"
#include "source/source.h"
#include "status/status.h"

/*
 * One macro definition. All pointers (name, params[i], replacement, and each
 * replacement token's spelling) are arena/interner owned by the preprocessor
 * and outlive the table entry. A value-ish record: the table stores it by
 * pointer so lookups can hand back a stable address.
 */
typedef struct qcc_macro {
    const char       *name;               /* Interned macro name.               */
    unsigned          is_function_like : 1; /* `NAME(...)` vs plain `NAME`.      */
    unsigned          is_variadic : 1;    /* Parameter list ended with `...`.    */

    const char      **params;             /* Interned parameter names, or NULL.  */
    size_t            param_count;        /* Number of named parameters.         */

    const qcc_ptok   *replacement;        /* Replacement list (may be empty).    */
    size_t            replacement_count;

    const qcc_source *def_source;         /* Where defined (redefinition note).  */
    size_t            def_offset;
} qcc_macro;

/* One open-addressing slot. An empty slot has key == NULL. */
typedef struct qcc_macro_slot {
    const char *key;    /* Interned macro name; NULL if empty.                  */
    qcc_macro  *macro;  /* Arena-owned record; valid iff key != NULL.           */
} qcc_macro_slot;

/*
 * The macro table: an open-addressing hash map (linear probing, power-of-two
 * capacity) from interned name to macro record. The bucket array is heap-owned
 * (seed allocator); the macro records and their contents live in the arena.
 */
typedef struct qcc_macro_table {
    qcc_macro_slot *slots;
    size_t          bucket_count;
    size_t          count;
} qcc_macro_table;

/* Initialize an empty table (allocates the initial bucket array). Returns
   QCC_OK or QCC_ERR_OUT_OF_MEMORY / QCC_ERR_INVALID_ARGUMENT. */
qcc_status qcc_macro_table_init(qcc_macro_table *table);

/* Free the bucket array and zero the table. Macro records are arena-owned and
   are NOT freed here. Idempotent and NULL-safe. */
void qcc_macro_table_dispose(qcc_macro_table *table);

/*
 * Look up `name` (an interned pointer). Returns the macro record, or NULL if no
 * macro by that name is currently defined.
 */
qcc_macro *qcc_macro_lookup(const qcc_macro_table *table, const char *name);

/*
 * Insert or replace the definition for macro->name. The table stores the given
 * pointer (caller keeps the record arena-allocated and stable). Returns QCC_OK,
 * or QCC_ERR_OUT_OF_MEMORY (table growth failed) / QCC_ERR_INVALID_ARGUMENT.
 */
qcc_status qcc_macro_put(qcc_macro_table *table, qcc_macro *macro);

/*
 * Remove the definition for `name` (interned). Returns 1 if a definition was
 * removed, 0 if none existed. Removal preserves the probe-chain invariant via
 * backward-shift deletion so later lookups still find their entries.
 */
int qcc_macro_remove(qcc_macro_table *table, const char *name);

/* Number of macros currently defined. */
size_t qcc_macro_count(const qcc_macro_table *table);

/*
 * Return 1 if `a` and `b` are identical definitions per §6.10.3 ¶1-2: same form
 * (object/function-like), same variadic-ness, same parameter names in order,
 * and replacement lists equal token-for-token (kind, punctuator, interned
 * spelling) with identical white-space separation. Returns 0 otherwise. NULL
 * arguments compare unequal (return 0) unless both are NULL (return 1).
 */
int qcc_macro_identical(const qcc_macro *a, const qcc_macro *b);

#endif /* QCC_PP_INTERNAL_MACRO_H */
