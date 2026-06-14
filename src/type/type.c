/*
 * qcc — C type representation (implementation).
 *
 * See type.h and ADR-0020. Basic types are lazily created singletons cached in
 * the context (so an unqualified basic type compares by pointer); everything else
 * is a fresh arena node compared structurally. Sizes and alignments are the
 * x86-64 System V LP64 values, the project's single source of truth for them.
 */
#include "type/type.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

qcc_status qcc_type_ctx_init(qcc_type_ctx *ctx)
{
    if (ctx == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&ctx->arena, 0);
    for (size_t i = 0; i < QCC_TYPE_BASIC_END; ++i) {
        ctx->basics[i] = NULL;
    }
    return QCC_OK;
}

void qcc_type_ctx_dispose(qcc_type_ctx *ctx)
{
    if (ctx != NULL) {
        qcc_arena_dispose(&ctx->arena);
    }
}

static qcc_type *type_alloc(qcc_type_ctx *ctx, qcc_type_kind kind)
{
    qcc_type *t = (qcc_type *)qcc_arena_calloc(&ctx->arena, 1, sizeof(*t), 0);
    if (t != NULL) {
        t->kind = kind;
    }
    return t;
}

const qcc_type *qcc_type_basic(qcc_type_ctx *ctx, qcc_type_kind kind)
{
    if (ctx == NULL || (unsigned)kind >= (unsigned)QCC_TYPE_BASIC_END) {
        return NULL;
    }
    if (ctx->basics[kind] == NULL) {
        ctx->basics[kind] = type_alloc(ctx, kind);
    }
    return ctx->basics[kind];
}

const qcc_type *qcc_type_qualified(qcc_type_ctx *ctx, const qcc_type *base,
                                   unsigned quals)
{
    if (ctx == NULL || base == NULL) {
        return NULL;
    }
    unsigned merged = base->qualifiers | quals;
    if (merged == base->qualifiers) {
        return base;
    }
    qcc_type *t = type_alloc(ctx, base->kind);
    if (t == NULL) {
        return NULL;
    }
    *t            = *base; /* Copy all constituents, then re-qualify. */
    t->qualifiers = merged;
    return t;
}

const qcc_type *qcc_type_pointer(qcc_type_ctx *ctx, const qcc_type *pointee,
                                 unsigned quals)
{
    if (ctx == NULL || pointee == NULL) {
        return NULL;
    }
    qcc_type *t = type_alloc(ctx, QCC_TYPE_POINTER);
    if (t != NULL) {
        t->pointee    = pointee;
        t->qualifiers = quals;
    }
    return t;
}

const qcc_type *qcc_type_array(qcc_type_ctx *ctx, const qcc_type *element,
                               uint64_t len, int complete)
{
    if (ctx == NULL || element == NULL) {
        return NULL;
    }
    qcc_type *t = type_alloc(ctx, QCC_TYPE_ARRAY);
    if (t != NULL) {
        t->element        = element;
        t->array_complete = complete ? 1 : 0;
        t->array_len      = complete ? len : 0;
    }
    return t;
}

const qcc_type *qcc_type_function(qcc_type_ctx *ctx, const qcc_type *ret,
                                  const qcc_type *const *params, size_t count,
                                  int variadic)
{
    if (ctx == NULL || ret == NULL || (count != 0 && params == NULL)) {
        return NULL;
    }
    qcc_type *t = type_alloc(ctx, QCC_TYPE_FUNCTION);
    if (t == NULL) {
        return NULL;
    }
    t->ret      = ret;
    t->variadic = variadic ? 1 : 0;
    if (count != 0) {
        const qcc_type **owned = (const qcc_type **)qcc_arena_memdup(
            &ctx->arena, params, count * sizeof(*params), 0);
        if (owned == NULL) {
            return NULL;
        }
        t->params      = owned;
        t->param_count = count;
    }
    return t;
}

const qcc_type *qcc_type_tagged(qcc_type_ctx *ctx, qcc_type_kind kind,
                                const char *tag, size_t tag_len, int complete)
{
    if (ctx == NULL ||
        (kind != QCC_TYPE_STRUCT && kind != QCC_TYPE_UNION &&
         kind != QCC_TYPE_ENUM)) {
        return NULL;
    }
    qcc_type *t = type_alloc(ctx, kind);
    if (t == NULL) {
        return NULL;
    }
    t->complete = complete ? 1 : 0;
    if (tag != NULL && tag_len != 0) {
        char *owned = (char *)qcc_arena_memdup(&ctx->arena, tag, tag_len, 1);
        if (owned == NULL) {
            return NULL;
        }
        t->tag     = owned;
        t->tag_len = tag_len;
    }
    return t;
}

int qcc_type_is_integer(const qcc_type *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case QCC_TYPE_BOOL:
    case QCC_TYPE_CHAR:  case QCC_TYPE_SCHAR:  case QCC_TYPE_UCHAR:
    case QCC_TYPE_SHORT: case QCC_TYPE_USHORT:
    case QCC_TYPE_INT:   case QCC_TYPE_UINT:
    case QCC_TYPE_LONG:  case QCC_TYPE_ULONG:
    case QCC_TYPE_LLONG: case QCC_TYPE_ULLONG:
    case QCC_TYPE_ENUM: /* §6.2.5 ¶17: an enumeration is an integer type. */
        return 1;
    default:
        return 0;
    }
}

int qcc_type_is_floating(const qcc_type *t)
{
    return t != NULL && (t->kind == QCC_TYPE_FLOAT || t->kind == QCC_TYPE_DOUBLE ||
                         t->kind == QCC_TYPE_LDOUBLE);
}

int qcc_type_is_arithmetic(const qcc_type *t)
{
    return qcc_type_is_integer(t) || qcc_type_is_floating(t);
}

int qcc_type_is_scalar(const qcc_type *t)
{
    return qcc_type_is_arithmetic(t) || (t != NULL && t->kind == QCC_TYPE_POINTER);
}

int qcc_type_is_signed_integer(const qcc_type *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case QCC_TYPE_CHAR: /* signed on the target (ADR-0018). */
    case QCC_TYPE_SCHAR:
    case QCC_TYPE_SHORT:
    case QCC_TYPE_INT:
    case QCC_TYPE_LONG:
    case QCC_TYPE_LLONG:
    case QCC_TYPE_ENUM:
        return 1;
    default:
        return 0;
    }
}

int qcc_type_is_unsigned_integer(const qcc_type *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case QCC_TYPE_BOOL:
    case QCC_TYPE_UCHAR:
    case QCC_TYPE_USHORT:
    case QCC_TYPE_UINT:
    case QCC_TYPE_ULONG:
    case QCC_TYPE_ULLONG:
        return 1;
    default:
        return 0;
    }
}

int qcc_type_equal(const qcc_type *a, const qcc_type *b)
{
    if (a == b) {
        return 1; /* Same pointer (covers both NULL and basic singletons). */
    }
    if (a == NULL || b == NULL || a->kind != b->kind ||
        a->qualifiers != b->qualifiers) {
        return 0;
    }
    switch (a->kind) {
    case QCC_TYPE_POINTER:
        return qcc_type_equal(a->pointee, b->pointee);
    case QCC_TYPE_ARRAY:
        if (!qcc_type_equal(a->element, b->element)) {
            return 0;
        }
        /* §6.2.7: compatible if elements compatible and, when both bounds are
           known, equal; an unknown bound is compatible with any. */
        if (a->array_complete && b->array_complete) {
            return a->array_len == b->array_len;
        }
        return 1;
    case QCC_TYPE_FUNCTION:
        if (!qcc_type_equal(a->ret, b->ret) || a->variadic != b->variadic ||
            a->param_count != b->param_count) {
            return 0;
        }
        for (size_t i = 0; i < a->param_count; ++i) {
            if (!qcc_type_equal(a->params[i], b->params[i])) {
                return 0;
            }
        }
        return 1;
    case QCC_TYPE_STRUCT:
    case QCC_TYPE_UNION:
    case QCC_TYPE_ENUM:
        /* Tagged types match by tag (anonymous distinct types only equal by
           identity, handled above). */
        return a->tag_len == b->tag_len && a->tag_len != 0 &&
               memcmp(a->tag, b->tag, a->tag_len) == 0;
    default:
        return 1; /* Basic kinds: kind + qualifiers already matched. */
    }
}

uint64_t qcc_type_size(const qcc_type *t)
{
    if (t == NULL) {
        return 0;
    }
    switch (t->kind) {
    case QCC_TYPE_BOOL:
    case QCC_TYPE_CHAR:  case QCC_TYPE_SCHAR:  case QCC_TYPE_UCHAR:  return 1;
    case QCC_TYPE_SHORT: case QCC_TYPE_USHORT:                      return 2;
    case QCC_TYPE_INT:   case QCC_TYPE_UINT:                        return 4;
    case QCC_TYPE_LONG:  case QCC_TYPE_ULONG:                       return 8;
    case QCC_TYPE_LLONG: case QCC_TYPE_ULLONG:                      return 8;
    case QCC_TYPE_FLOAT:                                            return 4;
    case QCC_TYPE_DOUBLE:                                           return 8;
    case QCC_TYPE_LDOUBLE:                                          return 16;
    case QCC_TYPE_POINTER:                                          return 8;
    case QCC_TYPE_ENUM:                                             return 4;
    case QCC_TYPE_ARRAY:
        return t->array_complete ? t->array_len * qcc_type_size(t->element) : 0;
    case QCC_TYPE_VOID:
    case QCC_TYPE_FUNCTION:
    case QCC_TYPE_STRUCT:
    case QCC_TYPE_UNION: /* No object size yet (incomplete / not laid out). */
    default:
        return 0;
    }
}

uint64_t qcc_type_align(const qcc_type *t)
{
    if (t == NULL) {
        return 0;
    }
    if (t->kind == QCC_TYPE_ARRAY) {
        return qcc_type_align(t->element); /* §6.2.8: array aligns as element. */
    }
    return qcc_type_size(t); /* Scalars/pointer: align == size on this target. */
}

const char *qcc_type_kind_name(qcc_type_kind kind)
{
    switch (kind) {
    case QCC_TYPE_VOID:    return "void";
    case QCC_TYPE_BOOL:    return "_Bool";
    case QCC_TYPE_CHAR:    return "char";
    case QCC_TYPE_SCHAR:   return "signed char";
    case QCC_TYPE_UCHAR:   return "unsigned char";
    case QCC_TYPE_SHORT:   return "short";
    case QCC_TYPE_USHORT:  return "unsigned short";
    case QCC_TYPE_INT:     return "int";
    case QCC_TYPE_UINT:    return "unsigned int";
    case QCC_TYPE_LONG:    return "long";
    case QCC_TYPE_ULONG:   return "unsigned long";
    case QCC_TYPE_LLONG:   return "long long";
    case QCC_TYPE_ULLONG:  return "unsigned long long";
    case QCC_TYPE_FLOAT:   return "float";
    case QCC_TYPE_DOUBLE:  return "double";
    case QCC_TYPE_LDOUBLE: return "long double";
    case QCC_TYPE_POINTER: return "pointer";
    case QCC_TYPE_ARRAY:   return "array";
    case QCC_TYPE_FUNCTION: return "function";
    case QCC_TYPE_STRUCT:  return "struct";
    case QCC_TYPE_UNION:   return "union";
    case QCC_TYPE_ENUM:    return "enum";
    case QCC_TYPE_KIND_COUNT: break;
    }
    return "unknown";
}

/* A growable text buffer for the printer (seed allocator, transient). */
typedef struct strbuf {
    char  *data;
    size_t len;
    size_t cap;
} strbuf;

static int sb_reserve(strbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) {
        return 1;
    }
    size_t ncap = (b->cap == 0) ? 64u : b->cap;
    while (ncap < b->len + extra + 1) {
        ncap *= 2u;
    }
    char *grown = (char *)realloc(b->data, ncap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap  = ncap;
    return 1;
}

static int sb_putn(strbuf *b, const char *s, size_t n)
{
    if (!sb_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_puts(strbuf *b, const char *s)
{
    return sb_putn(b, s, strlen(s));
}

static int emit_quals(strbuf *b, unsigned q)
{
    if ((q & QCC_QUAL_CONST) && !sb_puts(b, "const ")) {
        return 0;
    }
    if ((q & QCC_QUAL_VOLATILE) && !sb_puts(b, "volatile ")) {
        return 0;
    }
    if ((q & QCC_QUAL_RESTRICT) && !sb_puts(b, "restrict ")) {
        return 0;
    }
    if ((q & QCC_QUAL_ATOMIC) && !sb_puts(b, "_Atomic ")) {
        return 0;
    }
    return 1;
}

static int emit_type(strbuf *b, const qcc_type *t)
{
    char num[32];

    if (!emit_quals(b, t->qualifiers)) {
        return 0;
    }

    switch (t->kind) {
    case QCC_TYPE_POINTER:
        return sb_puts(b, "pointer to ") && emit_type(b, t->pointee);
    case QCC_TYPE_ARRAY:
        if (t->array_complete) {
            snprintf(num, sizeof(num), "array[%llu] of ",
                     (unsigned long long)t->array_len);
            return sb_puts(b, num) && emit_type(b, t->element);
        }
        return sb_puts(b, "array[] of ") && emit_type(b, t->element);
    case QCC_TYPE_FUNCTION: {
        if (!sb_puts(b, "function(")) {
            return 0;
        }
        for (size_t i = 0; i < t->param_count; ++i) {
            if ((i != 0 && !sb_puts(b, ", ")) || !emit_type(b, t->params[i])) {
                return 0;
            }
        }
        if (t->variadic &&
            !sb_puts(b, t->param_count != 0 ? ", ..." : "...")) {
            return 0;
        }
        return sb_puts(b, ") returning ") && emit_type(b, t->ret);
    }
    case QCC_TYPE_STRUCT:
    case QCC_TYPE_UNION:
    case QCC_TYPE_ENUM:
        return sb_puts(b, qcc_type_kind_name(t->kind)) && sb_puts(b, " ") &&
               (t->tag != NULL ? sb_putn(b, t->tag, t->tag_len)
                               : sb_puts(b, "<anonymous>"));
    default:
        return sb_puts(b, qcc_type_kind_name(t->kind)); /* basic kinds */
    }
}

qcc_status qcc_type_print(const qcc_type *t, char **out, size_t *len)
{
    if (t == NULL || out == NULL || len == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    strbuf b = { NULL, 0, 0 };
    if (!emit_type(&b, t) || !sb_reserve(&b, 0)) {
        free(b.data);
        return QCC_ERR_OUT_OF_MEMORY;
    }
    b.data[b.len] = '\0';
    *out = b.data;
    *len = b.len;
    return QCC_OK;
}
