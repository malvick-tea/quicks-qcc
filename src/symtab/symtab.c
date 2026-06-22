/*
 * qcc — symbol table and scopes (implementation).
 *
 * See symtab.h and ADR-0021. One hash table holds every live binding; a binding
 * is pushed onto its bucket head (so the innermost is found first) and onto its
 * scope's own list (so pop_scope unlinks exactly that scope's symbols). Names are
 * hashed with FNV-1a and copied into the arena, so the table owns its keys.
 */
#include "symtab/symtab.h"

#include <stdlib.h>
#include <string.h>

enum {
    SYMTAB_INITIAL_BUCKETS = 256u, /* Power of two; chains grow, no resize yet. */
    SYMTAB_INITIAL_SCOPES  = 8u
};

/* FNV-1a, 64-bit (the hash qas's symbol table uses). */
static uint64_t hash_name(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull; /* 14695981039346656037 mod 2^64. */
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

qcc_status qcc_symtab_init(qcc_symtab *tab)
{
    if (tab == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&tab->arena, 0);
    tab->bucket_count = SYMTAB_INITIAL_BUCKETS;
    tab->symbol_count = 0;
    tab->scope_count  = 0;
    tab->scope_cap    = SYMTAB_INITIAL_SCOPES;

    tab->buckets = (qcc_symbol **)calloc(tab->bucket_count, sizeof(*tab->buckets));
    tab->scopes  = (qcc_scope_frame *)malloc(tab->scope_cap * sizeof(*tab->scopes));
    if (tab->buckets == NULL || tab->scopes == NULL) {
        qcc_symtab_dispose(tab);
        return QCC_ERR_OUT_OF_MEMORY;
    }

    /* Open the file scope (§6.2.1). */
    tab->scopes[0].kind    = QCC_SCOPE_FILE;
    tab->scopes[0].symbols = NULL;
    tab->scope_count       = 1;
    return QCC_OK;
}

void qcc_symtab_dispose(qcc_symtab *tab)
{
    if (tab == NULL) {
        return;
    }
    free(tab->buckets);
    free(tab->scopes);
    tab->buckets = NULL;
    tab->scopes  = NULL;
    qcc_arena_dispose(&tab->arena);
    tab->bucket_count = 0;
    tab->symbol_count = 0;
    tab->scope_count  = 0;
    tab->scope_cap    = 0;
}

qcc_status qcc_symtab_push_scope(qcc_symtab *tab, qcc_scope_kind kind)
{
    if (tab == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (tab->scope_count == tab->scope_cap) {
        size_t           ncap  = tab->scope_cap * 2u;
        qcc_scope_frame *grown = (qcc_scope_frame *)realloc(
            tab->scopes, ncap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        tab->scopes    = grown;
        tab->scope_cap = ncap;
    }
    tab->scopes[tab->scope_count].kind    = kind;
    tab->scopes[tab->scope_count].symbols = NULL;
    ++tab->scope_count;
    return QCC_OK;
}

/* Unlink `sym` from its hash bucket. */
static void unlink_from_bucket(qcc_symtab *tab, qcc_symbol *sym)
{
    size_t       idx = (size_t)(sym->hash & (tab->bucket_count - 1));
    qcc_symbol **pp  = &tab->buckets[idx];
    while (*pp != NULL) {
        if (*pp == sym) {
            *pp = sym->bucket_next;
            return;
        }
        pp = &(*pp)->bucket_next;
    }
}

void qcc_symtab_pop_scope(qcc_symtab *tab)
{
    if (tab == NULL || tab->scope_count <= 1) {
        return; /* The file scope is never popped. */
    }
    qcc_scope_frame *frame = &tab->scopes[tab->scope_count - 1];
    for (qcc_symbol *s = frame->symbols; s != NULL; s = s->scope_next) {
        unlink_from_bucket(tab, s);
        --tab->symbol_count;
    }
    --tab->scope_count;
}

unsigned qcc_symtab_depth(const qcc_symtab *tab)
{
    return (tab != NULL && tab->scope_count > 0)
               ? (unsigned)(tab->scope_count - 1)
               : 0u;
}

/*
 * The shared insertion core. `enum_value` is meaningful only for an enum constant
 * (kind == QCC_SYM_ENUM_CONST) and 0 for every other binding. Both public entry
 * points funnel here so the bucket/scope linking lives in exactly one place.
 */
static qcc_status insert_symbol(qcc_symtab *tab, const char *name, size_t name_len,
                                qcc_sym_namespace ns, qcc_sym_kind kind,
                                const qcc_type *type, int64_t enum_value,
                                const struct qcc_source *source, size_t offset,
                                uint32_t line, uint32_t column,
                                const qcc_symbol **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (tab == NULL || name == NULL || tab->scope_count == 0) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_symbol *sym = (qcc_symbol *)qcc_arena_calloc(&tab->arena, 1, sizeof(*sym), 0);
    if (sym == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    char *owned = (char *)qcc_arena_memdup(&tab->arena, name, name_len, 1);
    if (owned == NULL && name_len != 0) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    sym->name       = (name_len != 0) ? owned : "";
    sym->name_len   = name_len;
    sym->ns         = ns;
    sym->kind       = kind;
    sym->type       = type;
    sym->enum_value = enum_value;
    sym->source     = source;
    sym->offset     = offset;
    sym->line       = line;
    sym->column     = column;
    sym->depth      = (unsigned)(tab->scope_count - 1);
    sym->hash       = hash_name(name, name_len);

    size_t idx       = (size_t)(sym->hash & (tab->bucket_count - 1));
    sym->bucket_next = tab->buckets[idx];
    tab->buckets[idx] = sym;

    qcc_scope_frame *frame = &tab->scopes[tab->scope_count - 1];
    sym->scope_next = frame->symbols;
    frame->symbols  = sym;

    ++tab->symbol_count;
    if (out != NULL) {
        *out = sym;
    }
    return QCC_OK;
}

qcc_status qcc_symtab_insert(qcc_symtab *tab, const char *name, size_t name_len,
                             qcc_sym_namespace ns, qcc_sym_kind kind,
                             const qcc_type *type, const struct qcc_source *source,
                             size_t offset, uint32_t line, uint32_t column,
                             const qcc_symbol **out)
{
    return insert_symbol(tab, name, name_len, ns, kind, type, 0, source, offset,
                         line, column, out);
}

qcc_status qcc_symtab_insert_enum_const(qcc_symtab *tab, const char *name,
                                        size_t name_len, const qcc_type *type,
                                        int64_t value,
                                        const struct qcc_source *source,
                                        size_t offset, uint32_t line,
                                        uint32_t column, const qcc_symbol **out)
{
    return insert_symbol(tab, name, name_len, QCC_NS_ORDINARY, QCC_SYM_ENUM_CONST,
                         type, value, source, offset, line, column, out);
}

const qcc_symbol *qcc_symtab_lookup(const qcc_symtab *tab, const char *name,
                                    size_t name_len, qcc_sym_namespace ns)
{
    if (tab == NULL || name == NULL || tab->buckets == NULL) {
        return NULL;
    }
    uint64_t h   = hash_name(name, name_len);
    size_t   idx = (size_t)(h & (tab->bucket_count - 1));
    /* Innermost binding is at the head (most recently inserted), so the first
       name/namespace match is the visible one (§6.2.1 most-closely-nested). */
    for (const qcc_symbol *s = tab->buckets[idx]; s != NULL; s = s->bucket_next) {
        if (s->ns == ns && s->name_len == name_len &&
            (name_len == 0 || memcmp(s->name, name, name_len) == 0)) {
            return s;
        }
    }
    return NULL;
}

const qcc_symbol *qcc_symtab_lookup_current_scope(const qcc_symtab *tab,
                                                  const char *name,
                                                  size_t name_len,
                                                  qcc_sym_namespace ns)
{
    if (tab == NULL || name == NULL || tab->scope_count == 0) {
        return NULL;
    }
    const qcc_scope_frame *frame = &tab->scopes[tab->scope_count - 1];
    for (const qcc_symbol *s = frame->symbols; s != NULL; s = s->scope_next) {
        if (s->ns == ns && s->name_len == name_len &&
            (name_len == 0 || memcmp(s->name, name, name_len) == 0)) {
            return s;
        }
    }
    return NULL;
}

int qcc_symtab_is_typedef_name(const qcc_symtab *tab, const char *name,
                               size_t name_len)
{
    const qcc_symbol *s = qcc_symtab_lookup(tab, name, name_len, QCC_NS_ORDINARY);
    return s != NULL && s->kind == QCC_SYM_TYPEDEF;
}

const char *qcc_sym_kind_name(qcc_sym_kind kind)
{
    switch (kind) {
    case QCC_SYM_TYPEDEF:    return "typedef";
    case QCC_SYM_OBJECT:     return "object";
    case QCC_SYM_FUNCTION:   return "function";
    case QCC_SYM_ENUM_CONST: return "enum-constant";
    case QCC_SYM_TAG:        return "tag";
    }
    return "unknown";
}
