/*
 * qcc — symbol table and scopes (ISO C11 §6.2.1, §6.2.3; design in ADR-0021)
 *
 * Responsibility
 * Track the names a C program declares and the scope each is visible in, so the
 * declaration parser (ADR-0019 Unit 2) can register declared identifiers and —
 * critically — answer the question that makes the C grammar parseable at all: is
 * this identifier a `typedef`-name in the current scope (§6.7.8)? Models C's
 * block scoping (§6.2.1, inner hides outer) and the separate name spaces of
 * §6.2.3 (ordinary identifiers vs. tags).
 *
 * Structure
 *   One hash table holding every live binding at once, each bucket a chain ordered
 *   innermost-first, plus a per-scope list so closing a scope removes exactly its
 *   symbols (ADR-0021). lookup is O(1) expected; is_typedef_name — on the parser's
 *   hottest path — is just an ordinary-space lookup.
 *
 * Ownership
 *   The table owns an arena holding the symbols and copies of their names, so it
 *   is independent of the token stream's lifetime. A symbol's `type` is borrowed
 *   (from a qcc_type_ctx, ADR-0020) and its `source` from the producing convert;
 *   both must outlive the table.
 *
 * Standard: ISO/IEC 9899 (C11) §6.2.1, §6.2.3, §6.7.8. Builds on `type`, `arena`,
 * `status`.
 */
#ifndef QCC_SYMTAB_SYMTAB_H
#define QCC_SYMTAB_SYMTAB_H

#include <stddef.h>
#include <stdint.h>

#include "arena/arena.h"
#include "status/status.h"
#include "type/type.h"

struct qcc_source; /* Provenance only; no source.h dependency (ADR-0008). */

/* The name spaces of §6.2.3 this module carries (labels and struct members are
   handled elsewhere). An identifier is keyed by (name, name space). */
typedef enum qcc_sym_namespace {
    QCC_NS_ORDINARY = 0, /* objects, functions, typedef-names, enum constants */
    QCC_NS_TAG           /* struct/union/enum tags                            */
} qcc_sym_namespace;

/* What an ordinary identifier denotes (§6.2.1); a tag's kind is QCC_SYM_TAG. */
typedef enum qcc_sym_kind {
    QCC_SYM_TYPEDEF = 0, /* a typedef-name (§6.7.8)        */
    QCC_SYM_OBJECT,      /* an object (variable)           */
    QCC_SYM_FUNCTION,    /* a function                     */
    QCC_SYM_ENUM_CONST,  /* an enumeration constant        */
    QCC_SYM_TAG          /* a struct/union/enum tag        */
} qcc_sym_kind;

/* The kind of a scope (§6.2.1). */
typedef enum qcc_scope_kind {
    QCC_SCOPE_FILE = 0,  /* the outermost scope                              */
    QCC_SCOPE_BLOCK,     /* a { } block                                      */
    QCC_SCOPE_PROTOTYPE  /* a function prototype's parameter list (§6.2.1 ¶4)*/
} qcc_scope_kind;

/*
 * One binding. The first fields are the caller's to read; `depth`, `hash`, and the
 * two chain pointers are private bookkeeping. A symbol is arena-owned; never freed
 * individually.
 */
typedef struct qcc_symbol {
    const char              *name;     /* Arena-owned copy; length in name_len.  */
    size_t                   name_len;
    qcc_sym_namespace        ns;
    qcc_sym_kind             kind;
    const qcc_type          *type;     /* Borrowed (from a qcc_type_ctx).        */
    const struct qcc_source *source;   /* Provenance; may be NULL.               */
    size_t                   offset;
    uint32_t                 line;
    uint32_t                 column;

    unsigned                 depth;        /* Scope depth (0 = file scope).      */
    uint64_t                 hash;         /* Cached name hash (private).         */
    struct qcc_symbol       *bucket_next;  /* Hash-bucket chain (private).        */
    struct qcc_symbol       *scope_next;   /* This scope's symbol list (private). */
} qcc_symbol;

/* One open scope. Private; the stack lives inside qcc_symtab. */
typedef struct qcc_scope_frame {
    qcc_scope_kind kind;
    qcc_symbol    *symbols; /* Head of this scope's scope_next list. */
} qcc_scope_frame;

/*
 * The symbol table. Treat the fields as private; use the functions. After init it
 * already has the file scope open (depth 0).
 */
typedef struct qcc_symtab {
    qcc_arena         arena;
    qcc_symbol      **buckets;
    size_t            bucket_count;  /* Power of two; mask is bucket_count - 1. */
    size_t            symbol_count;
    qcc_scope_frame  *scopes;        /* The scope stack (seed-allocated).       */
    size_t            scope_count;   /* >= 1 (the file scope).                  */
    size_t            scope_cap;
} qcc_symtab;

/* Initialize a table with the file scope open. Returns QCC_OK or
   QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY; on failure safe to dispose. */
qcc_status qcc_symtab_init(qcc_symtab *tab);

/* Release the arena and the table's own allocations. Idempotent and NULL-safe. */
void qcc_symtab_dispose(qcc_symtab *tab);

/* Open a nested scope of `kind`. Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY. */
qcc_status qcc_symtab_push_scope(qcc_symtab *tab, qcc_scope_kind kind);

/* Close the innermost scope, removing its bindings (the file scope is never
   popped). NULL-safe; a no-op at file scope. */
void qcc_symtab_pop_scope(qcc_symtab *tab);

/* The depth of the current (innermost) scope; 0 at file scope. */
unsigned qcc_symtab_depth(const qcc_symtab *tab);

/*
 * Insert a binding in the current scope. The name bytes are copied into the
 * table's arena. The caller decides the §6.2.1/§6.7 redeclaration policy — check
 * qcc_symtab_lookup_current_scope first if a duplicate should be diagnosed; this
 * function inserts unconditionally and the new binding shadows any earlier one.
 * On success *out (if non-NULL) receives the inserted symbol. Returns QCC_OK or
 * QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY.
 */
qcc_status qcc_symtab_insert(qcc_symtab *tab, const char *name, size_t name_len,
                             qcc_sym_namespace ns, qcc_sym_kind kind,
                             const qcc_type *type, const struct qcc_source *source,
                             size_t offset, uint32_t line, uint32_t column,
                             const qcc_symbol **out);

/* Look up `name` in name space `ns`, innermost scope outward; NULL if unbound. */
const qcc_symbol *qcc_symtab_lookup(const qcc_symtab *tab, const char *name,
                                    size_t name_len, qcc_sym_namespace ns);

/* Look up `name`/`ns` only in the current scope (for redeclaration checks). */
const qcc_symbol *qcc_symtab_lookup_current_scope(const qcc_symtab *tab,
                                                  const char *name,
                                                  size_t name_len,
                                                  qcc_sym_namespace ns);

/* Is `name` a visible typedef-name (§6.7.8)? The parser's disambiguation hook. */
int qcc_symtab_is_typedef_name(const qcc_symtab *tab, const char *name,
                               size_t name_len);

/* Stable lowercase name of a symbol kind ("typedef", "object", "tag", …). */
const char *qcc_sym_kind_name(qcc_sym_kind kind);

#endif /* QCC_SYMTAB_SYMTAB_H */
