/*
 * Tests for the symtab module (ISO C11 §6.2.1, §6.2.3, §6.7.8; ADR-0021): scope
 * push/pop with inner-hides-outer, the ordinary/tag name spaces, the typedef
 * disambiguation hook, and current-scope redeclaration lookup.
 */
#include "qtest.h"

#include <string.h>

#include "symtab/symtab.h"
#include "type/type.h"

/* Convenience: insert an ordinary identifier of `kind` with type `t`. */
static const qcc_symbol *put(qcc_symtab *tab, const char *name, qcc_sym_kind kind,
                             const qcc_type *t)
{
    const qcc_symbol *sym = NULL;
    QTEST_CHECK_EQ_INT(qcc_symtab_insert(tab, name, strlen(name), QCC_NS_ORDINARY,
                                         kind, t, NULL, 0, 0, 0, &sym),
                       QCC_OK, "insert");
    return sym;
}

static const qcc_symbol *get(const qcc_symtab *tab, const char *name)
{
    return qcc_symtab_lookup(tab, name, strlen(name), QCC_NS_ORDINARY);
}

static void test_basic_insert_lookup(void)
{
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    QTEST_CHECK_EQ_INT(qcc_symtab_init(&tab), QCC_OK, "init");
    QTEST_CHECK_EQ_UINT(qcc_symtab_depth(&tab), 0, "file scope depth 0");

    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);
    put(&tab, "x", QCC_SYM_OBJECT, int_t);

    const qcc_symbol *x = get(&tab, "x");
    QTEST_CHECK_TRUE(x != NULL);
    if (x != NULL) {
        QTEST_CHECK_EQ_INT(x->kind, QCC_SYM_OBJECT, "kind");
        QTEST_CHECK_TRUE(x->type == int_t);
        QTEST_CHECK_EQ_UINT(x->depth, 0, "depth");
    }
    QTEST_CHECK_TRUE(get(&tab, "y") == NULL); /* unbound */

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_scope_hiding(void)
{
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);

    const qcc_type *int_t  = qcc_type_basic(&tc, QCC_TYPE_INT);
    const qcc_type *char_t = qcc_type_basic(&tc, QCC_TYPE_CHAR);

    put(&tab, "v", QCC_SYM_OBJECT, int_t); /* outer: int v */

    QTEST_CHECK_EQ_INT(qcc_symtab_push_scope(&tab, QCC_SCOPE_BLOCK), QCC_OK, "push");
    QTEST_CHECK_EQ_UINT(qcc_symtab_depth(&tab), 1, "depth 1");
    put(&tab, "v", QCC_SYM_OBJECT, char_t); /* inner: char v hides int v */

    QTEST_CHECK_TRUE(get(&tab, "v")->type == char_t); /* inner visible */

    qcc_symtab_pop_scope(&tab);
    QTEST_CHECK_EQ_UINT(qcc_symtab_depth(&tab), 0, "back to file scope");
    QTEST_CHECK_TRUE(get(&tab, "v")->type == int_t); /* outer visible again */

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_namespaces(void)
{
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);

    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);
    const qcc_type *st    = qcc_type_tagged(&tc, QCC_TYPE_STRUCT, "stat", 4, 1);

    /* The same name as a tag and an ordinary identifier coexist (§6.2.3). */
    const qcc_symbol *tag = NULL;
    qcc_symtab_insert(&tab, "stat", 4, QCC_NS_TAG, QCC_SYM_TAG, st, NULL, 0, 0, 0,
                      &tag);
    put(&tab, "stat", QCC_SYM_OBJECT, int_t);

    QTEST_CHECK_TRUE(qcc_symtab_lookup(&tab, "stat", 4, QCC_NS_TAG)->kind ==
                     QCC_SYM_TAG);
    QTEST_CHECK_TRUE(qcc_symtab_lookup(&tab, "stat", 4, QCC_NS_ORDINARY)->kind ==
                     QCC_SYM_OBJECT);

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_typedef_disambiguation(void)
{
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);

    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);
    put(&tab, "T", QCC_SYM_TYPEDEF, int_t); /* typedef int T; at file scope */

    QTEST_CHECK_TRUE(qcc_symtab_is_typedef_name(&tab, "T", 1));
    QTEST_CHECK_TRUE(!qcc_symtab_is_typedef_name(&tab, "U", 1));

    /* A nested object declaration of T makes it no longer a type name there. */
    qcc_symtab_push_scope(&tab, QCC_SCOPE_BLOCK);
    put(&tab, "T", QCC_SYM_OBJECT, int_t);
    QTEST_CHECK_TRUE(!qcc_symtab_is_typedef_name(&tab, "T", 1)); /* object hides */
    qcc_symtab_pop_scope(&tab);
    QTEST_CHECK_TRUE(qcc_symtab_is_typedef_name(&tab, "T", 1)); /* typedef again */

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_redeclaration_check(void)
{
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);
    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);

    put(&tab, "a", QCC_SYM_OBJECT, int_t);
    /* A second 'a' in the SAME scope is detectable by the caller. */
    QTEST_CHECK_TRUE(qcc_symtab_lookup_current_scope(&tab, "a", 1, QCC_NS_ORDINARY)
                     != NULL);

    qcc_symtab_push_scope(&tab, QCC_SCOPE_BLOCK);
    /* 'a' is not in THIS scope yet, though it is visible from the outer one. */
    QTEST_CHECK_TRUE(qcc_symtab_lookup_current_scope(&tab, "a", 1, QCC_NS_ORDINARY)
                     == NULL);
    QTEST_CHECK_TRUE(get(&tab, "a") != NULL); /* still visible via outer scope */
    qcc_symtab_pop_scope(&tab);

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_many_symbols(void)
{
    /* Exercise the hash buckets beyond their initial count with distinct names. */
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);
    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);

    char name[16];
    for (int i = 0; i < 500; ++i) {
        snprintf(name, sizeof(name), "sym%d", i);
        const qcc_symbol *s = NULL;
        QTEST_CHECK_EQ_INT(qcc_symtab_insert(&tab, name, strlen(name),
                                             QCC_NS_ORDINARY, QCC_SYM_OBJECT,
                                             int_t, NULL, 0, 0, 0, &s),
                           QCC_OK, "insert many");
    }
    /* Every one is findable. */
    int found = 1;
    for (int i = 0; i < 500; ++i) {
        snprintf(name, sizeof(name), "sym%d", i);
        if (qcc_symtab_lookup(&tab, name, strlen(name), QCC_NS_ORDINARY) == NULL) {
            found = 0;
        }
    }
    QTEST_CHECK_TRUE(found);
    QTEST_CHECK_TRUE(qcc_symtab_lookup(&tab, "sym999", 6, QCC_NS_ORDINARY) == NULL);

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

static void test_invalid_args(void)
{
    QTEST_CHECK_EQ_INT(qcc_symtab_init(NULL), QCC_ERR_INVALID_ARGUMENT, "null init");
    qcc_symtab_dispose(NULL);
    qcc_symtab_pop_scope(NULL);

    qcc_symtab tab;
    qcc_symtab_init(&tab);
    qcc_symtab_pop_scope(&tab); /* no-op at file scope */
    QTEST_CHECK_EQ_UINT(qcc_symtab_depth(&tab), 0, "still file scope");
    QTEST_CHECK_EQ_INT(qcc_symtab_insert(&tab, NULL, 0, QCC_NS_ORDINARY,
                                         QCC_SYM_OBJECT, NULL, NULL, 0, 0, 0, NULL),
                       QCC_ERR_INVALID_ARGUMENT, "null name");
    QTEST_CHECK_SPAN(qcc_sym_kind_name(QCC_SYM_TAG),
                     strlen(qcc_sym_kind_name(QCC_SYM_TAG)), "tag", "kind name");
    qcc_symtab_dispose(&tab);
}

static void test_enum_constant(void)
{
    /* qcc_symtab_insert_enum_const records the §6.7.2.2 value alongside the kind,
       and reads back through an ordinary-name-space lookup (including a negative
       value, which the int64_t field must preserve). */
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab tab;
    qcc_symtab_init(&tab);
    const qcc_type *int_t = qcc_type_basic(&tc, QCC_TYPE_INT);

    QTEST_CHECK_EQ_INT(qcc_symtab_insert_enum_const(&tab, "RED", 3, int_t, 0,
                                                    NULL, 0, 0, 0, NULL),
                       QCC_OK, "insert RED");
    QTEST_CHECK_EQ_INT(qcc_symtab_insert_enum_const(&tab, "LOW", 3, int_t, -1,
                                                    NULL, 0, 0, 0, NULL),
                       QCC_OK, "insert LOW");

    const qcc_symbol *red = get(&tab, "RED");
    QTEST_CHECK_TRUE(red != NULL && red->kind == QCC_SYM_ENUM_CONST);
    QTEST_CHECK_EQ_INT((int)red->enum_value, 0, "RED value");
    const qcc_symbol *low = get(&tab, "LOW");
    QTEST_CHECK_TRUE(low != NULL && low->kind == QCC_SYM_ENUM_CONST);
    QTEST_CHECK_EQ_INT((int)low->enum_value, -1, "LOW value");

    /* An enum constant is an ordinary identifier, not a typedef-name (§6.7.8). */
    QTEST_CHECK_TRUE(!qcc_symtab_is_typedef_name(&tab, "RED", 3));

    qcc_symtab_dispose(&tab);
    qcc_type_ctx_dispose(&tc);
}

int main(void)
{
    test_basic_insert_lookup();
    test_scope_hiding();
    test_namespaces();
    test_typedef_disambiguation();
    test_redeclaration_check();
    test_many_symbols();
    test_invalid_args();
    test_enum_constant();
    return qtest_report("symtab");
}
