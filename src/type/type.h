/*
 * qcc — C type representation (ISO C11 §6.2.5; design in ADR-0020)
 *
 * Responsibility
 * Model the types of a C program: the basic types (§6.2.5), the derived types
 * (pointer, array, function, structure, union), and enumerations, each able to
 * carry qualifiers (§6.7.3). This is the thing the declaration parser builds
 * (ADR-0019 Unit 2), semantic analysis queries (§6.2.7 compatibility, §6.3
 * conversions), `sizeof`/`_Alignof` measure, and codegen lowers. It also owns the
 * single source of truth for object sizes/alignment on the target (x86-64 System V
 * LP64, ADR-0001).
 *
 * Model (and why a canonical recursive node)
 *   One `qcc_type` tagged by `kind`, with per-kind fields ("valid iff kind == K",
 *   the style `qcc_token`/`qcc_expr` use). The basic arithmetic types are distinct
 *   kinds, following §6.2.5 (which enumerates them, and makes `char` a *third*
 *   type distinct from `signed char` and `unsigned char`, §6.2.5 ¶15); derived
 *   types point at their constituents. A `qcc_type_ctx` owns an arena and caches
 *   the basic types as singletons, so an unqualified basic type compares by
 *   pointer identity; derived and qualified types are compared structurally by
 *   qcc_type_equal (§6.2.7).
 *
 * Standard: ISO/IEC 9899 (C11) §6.2.5, §6.2.7, §6.7.3. Builds on `arena`,
 * `status`.
 */
#ifndef QCC_TYPE_TYPE_H
#define QCC_TYPE_TYPE_H

#include <stddef.h>
#include <stdint.h>

#include "arena/arena.h"
#include "status/status.h"

/*
 * The kind of a type. The basic types (§6.2.5 ¶1-10) are listed first and
 * contiguously so the context can cache them in an array indexed by kind; the
 * derived/tagged kinds follow. _Complex/_Imaginary are deferred.
 */
typedef enum qcc_type_kind {
    QCC_TYPE_VOID = 0,    /* void                                              */
    QCC_TYPE_BOOL,        /* _Bool                                             */
    QCC_TYPE_CHAR,        /* char (distinct from signed/unsigned, §6.2.5 ¶15)  */
    QCC_TYPE_SCHAR,       /* signed char                                       */
    QCC_TYPE_UCHAR,       /* unsigned char                                     */
    QCC_TYPE_SHORT,       /* short                                             */
    QCC_TYPE_USHORT,      /* unsigned short                                    */
    QCC_TYPE_INT,         /* int                                               */
    QCC_TYPE_UINT,        /* unsigned int                                      */
    QCC_TYPE_LONG,        /* long                                              */
    QCC_TYPE_ULONG,       /* unsigned long                                     */
    QCC_TYPE_LLONG,       /* long long                                         */
    QCC_TYPE_ULLONG,      /* unsigned long long                                */
    QCC_TYPE_FLOAT,       /* float                                             */
    QCC_TYPE_DOUBLE,      /* double                                            */
    QCC_TYPE_LDOUBLE,     /* long double                                       */

    QCC_TYPE_POINTER,     /* §6.2.5 ¶20: pointer to `pointee`.                 */
    QCC_TYPE_ARRAY,       /* §6.2.5 ¶20: array of `element`.                   */
    QCC_TYPE_FUNCTION,    /* §6.2.5 ¶20: function returning `ret`.             */
    QCC_TYPE_STRUCT,      /* §6.2.5 ¶20: structure (tag + completeness).       */
    QCC_TYPE_UNION,       /* §6.2.5 ¶20: union.                                */
    QCC_TYPE_ENUM,        /* §6.2.5 ¶16: enumeration.                          */

    QCC_TYPE_KIND_COUNT
} qcc_type_kind;

/* The first non-basic kind; the basic kinds are [0, QCC_TYPE_BASIC_END). */
#define QCC_TYPE_BASIC_END QCC_TYPE_POINTER

/* Type qualifiers (§6.7.3 ¶1), a bitmask on any type. */
enum {
    QCC_QUAL_NONE     = 0,
    QCC_QUAL_CONST    = 1u << 0, /* const                                       */
    QCC_QUAL_VOLATILE = 1u << 1, /* volatile                                    */
    QCC_QUAL_RESTRICT = 1u << 2, /* restrict (only on pointer types, §6.7.3 ¶2) */
    QCC_QUAL_ATOMIC   = 1u << 3  /* _Atomic (qualifier spelling, §6.7.3)        */
};

/*
 * One type. Arena-owned by a qcc_type_ctx; never freed individually. Treat as
 * immutable once built (constructors return `const qcc_type *`). Fields valid per
 * `kind`:
 *   qualifiers          : the §6.7.3 qualifier bitmask (any kind).
 *   pointee             : POINTER — the referenced type.
 *   element/array_len/array_complete : ARRAY — element type, element count (valid
 *                         iff array_complete), and whether the bound is known.
 *   ret/params/param_count/variadic  : FUNCTION — return type, parameter types in
 *                         order, and whether it ends with `, ...`.
 *   tag/tag_len/complete : STRUCT/UNION/ENUM — the tag name (NULL/0 if anonymous)
 *                         and whether the type has been defined yet.
 */
typedef struct qcc_type qcc_type;
struct qcc_type {
    qcc_type_kind          kind;
    unsigned               qualifiers;

    const qcc_type        *pointee;

    const qcc_type        *element;
    uint64_t               array_len;
    int                    array_complete;

    const qcc_type        *ret;
    const qcc_type *const *params;
    size_t                 param_count;
    int                    variadic;

    const char            *tag;
    size_t                 tag_len;
    int                    complete;
};

/*
 * The type owner: an arena plus a cache of the basic-type singletons. Treat the
 * fields as private; use the functions. Lives on the stack.
 */
typedef struct qcc_type_ctx {
    qcc_arena arena;
    qcc_type *basics[QCC_TYPE_BASIC_END]; /* Lazily created singletons. */
} qcc_type_ctx;

/* Initialize an empty context (lazy basic-type cache). Always succeeds; safe to
   dispose even after an early return. */
qcc_status qcc_type_ctx_init(qcc_type_ctx *ctx);

/* Release the arena. Every type the context produced becomes invalid. Idempotent
   and NULL-safe. */
void qcc_type_ctx_dispose(qcc_type_ctx *ctx);

/*
 * Type constructors. Each returns an arena-owned type, or NULL on out-of-memory
 * (or QCC_TYPE basic for a non-basic kind). Built types are immutable.
 */

/* The unqualified singleton for a basic kind (VOID..LDOUBLE). NULL if `kind` is
   not a basic kind. Same kind ⇒ same pointer (identity equality). */
const qcc_type *qcc_type_basic(qcc_type_ctx *ctx, qcc_type_kind kind);

/* `base` with `quals` added to its qualifiers (§6.7.3). Returns `base` itself if
   it already has them; otherwise a fresh qualified copy. */
const qcc_type *qcc_type_qualified(qcc_type_ctx *ctx, const qcc_type *base,
                                   unsigned quals);

/* Pointer to `pointee`, the pointer itself carrying `quals` (e.g. `int *const`). */
const qcc_type *qcc_type_pointer(qcc_type_ctx *ctx, const qcc_type *pointee,
                                 unsigned quals);

/* Array of `element`; `complete` says whether `len` is a known bound (`[]` ⇒ 0). */
const qcc_type *qcc_type_array(qcc_type_ctx *ctx, const qcc_type *element,
                               uint64_t len, int complete);

/* Function returning `ret` with `count` parameter types (copied) and an optional
   trailing `...`. */
const qcc_type *qcc_type_function(qcc_type_ctx *ctx, const qcc_type *ret,
                                  const qcc_type *const *params, size_t count,
                                  int variadic);

/* A struct/union/enum type with `tag` (NULL ⇒ anonymous, copied) and a
   completeness flag. `kind` must be STRUCT, UNION, or ENUM. */
const qcc_type *qcc_type_tagged(qcc_type_ctx *ctx, qcc_type_kind kind,
                                const char *tag, size_t tag_len, int complete);

/* Classification predicates (§6.2.5). */
int qcc_type_is_integer(const qcc_type *t);        /* incl. char, _Bool, enum  */
int qcc_type_is_floating(const qcc_type *t);       /* float/double/long double */
int qcc_type_is_arithmetic(const qcc_type *t);     /* integer or floating      */
int qcc_type_is_scalar(const qcc_type *t);         /* arithmetic or pointer    */
int qcc_type_is_signed_integer(const qcc_type *t); /* the signed integer types */
int qcc_type_is_unsigned_integer(const qcc_type *t);

/* Structural compatibility (§6.2.7) for the cases modeled so far: same kind and
   qualifiers, with constituent types compared recursively; tagged types match by
   tag. Returns 1 if `a` and `b` are the same type, else 0. NULL-safe (NULL only
   equals NULL). */
int qcc_type_equal(const qcc_type *a, const qcc_type *b);

/*
 * Object size and alignment in bytes on the x86-64 System V LP64 target (§6.5.3.4
 * sizeof / _Alignof). Returns 0 for a type with no object size (void, a function,
 * an incomplete array/struct/union); the caller diagnoses those.
 */
uint64_t qcc_type_size(const qcc_type *t);
uint64_t qcc_type_align(const qcc_type *t);

/* Stable lowercase spelling of a kind ("int", "unsigned long", "pointer", …). */
const char *qcc_type_kind_name(qcc_type_kind kind);

/*
 * Render a readable description of `t` — "const int", "pointer to int",
 * "array[3] of int", "function(int, int) returning int" — into a newly
 * heap-allocated NUL-terminated string (*out, caller frees) of length *len.
 * Deterministic, for diagnostics and tests. Returns QCC_OK or
 * QCC_ERR_OUT_OF_MEMORY / QCC_ERR_INVALID_ARGUMENT.
 */
qcc_status qcc_type_print(const qcc_type *t, char **out, size_t *len);

#endif /* QCC_TYPE_TYPE_H */
