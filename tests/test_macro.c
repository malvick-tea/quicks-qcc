/*
 * Tests for the preprocessor macro table (ISO C11 §6.10.3): put/lookup/remove,
 * table growth, the identical-redefinition test, and that backward-shift
 * deletion keeps every surviving entry findable. Macro records and their
 * replacement tokens are built in an arena, with names/spellings interned so
 * the table's pointer-identity keying works (as it does in the real pipeline).
 *
 * Includes the pp module's own internal header — legitimate for the module's
 * own tests (see test_hideset.c).
 */
#include "qtest.h"

#include <stdio.h>
#include <string.h>

#include "arena/arena.h"
#include "intern/intern.h"
#include "pp/internal/macro.h"
#include "pp/pp.h"

/* Build an identifier token with interned spelling. */
static qcc_ptok ident_tok(qcc_intern *in, const char *s, int leading_space)
{
    qcc_ptok t;
    memset(&t, 0, sizeof(t));
    t.kind          = QCC_PP_TOKEN_IDENTIFIER;
    t.spelling      = qcc_intern_str(in, s);
    t.spelling_len  = strlen(s);
    t.leading_space = leading_space ? 1u : 0u;
    return t;
}

/* Allocate an object-like macro with the given replacement list in the arena. */
static qcc_macro *obj_macro(qcc_arena *a, qcc_intern *in, const char *name,
                            const qcc_ptok *repl, size_t n)
{
    qcc_macro *m = (qcc_macro *)qcc_arena_alloc(a, sizeof(*m), _Alignof(qcc_macro));
    memset(m, 0, sizeof(*m));
    m->name              = qcc_intern_str(in, name);
    m->replacement       = (const qcc_ptok *)qcc_arena_memdup(
        a, repl, n * sizeof(qcc_ptok), _Alignof(qcc_ptok));
    m->replacement_count = n;
    return m;
}

static void test_put_lookup_remove(void)
{
    qcc_arena       a;
    qcc_intern      in;
    qcc_macro_table t;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);
    QTEST_CHECK_EQ_INT(qcc_macro_table_init(&t), QCC_OK, "table init");

    qcc_ptok repl[1] = { ident_tok(&in, "1", 0) };
    qcc_macro *x = obj_macro(&a, &in, "X", repl, 1);
    QTEST_CHECK_EQ_INT(qcc_macro_put(&t, x), QCC_OK, "put X");
    QTEST_CHECK_EQ_UINT(qcc_macro_count(&t), 1, "count 1");

    const char *x_name = qcc_intern_str(&in, "X");
    QTEST_CHECK_TRUE(qcc_macro_lookup(&t, x_name) == x);
    QTEST_CHECK_TRUE(qcc_macro_lookup(&t, qcc_intern_str(&in, "Y")) == NULL);

    QTEST_CHECK_EQ_INT(qcc_macro_remove(&t, x_name), 1, "remove X");
    QTEST_CHECK_TRUE(qcc_macro_lookup(&t, x_name) == NULL);
    QTEST_CHECK_EQ_UINT(qcc_macro_count(&t), 0, "count 0");
    QTEST_CHECK_EQ_INT(qcc_macro_remove(&t, x_name), 0, "remove absent");

    qcc_macro_table_dispose(&t);
    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

static void test_redefine_replaces(void)
{
    qcc_arena       a;
    qcc_intern      in;
    qcc_macro_table t;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);
    qcc_macro_table_init(&t);

    qcc_ptok r1[1] = { ident_tok(&in, "1", 0) };
    qcc_ptok r2[1] = { ident_tok(&in, "2", 0) };
    qcc_macro *v1 = obj_macro(&a, &in, "V", r1, 1);
    qcc_macro *v2 = obj_macro(&a, &in, "V", r2, 1);

    qcc_macro_put(&t, v1);
    qcc_macro_put(&t, v2); /* same name -> replaces, no count change */
    QTEST_CHECK_EQ_UINT(qcc_macro_count(&t), 1, "redefine keeps count 1");
    QTEST_CHECK_TRUE(qcc_macro_lookup(&t, qcc_intern_str(&in, "V")) == v2);

    qcc_macro_table_dispose(&t);
    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

static void test_identical(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    /* Two definitions "A B" with identical tokens and spacing -> identical. */
    qcc_ptok ra[2] = { ident_tok(&in, "A", 0), ident_tok(&in, "B", 1) };
    qcc_ptok rb[2] = { ident_tok(&in, "A", 0), ident_tok(&in, "B", 1) };
    qcc_macro *m1 = obj_macro(&a, &in, "M", ra, 2);
    qcc_macro *m2 = obj_macro(&a, &in, "M", rb, 2);
    QTEST_CHECK_EQ_INT(qcc_macro_identical(m1, m2), 1, "identical lists");

    /* Differing whitespace separation -> not identical (B with vs without). */
    qcc_ptok rc[2] = { ident_tok(&in, "A", 0), ident_tok(&in, "B", 0) };
    qcc_macro *m3 = obj_macro(&a, &in, "M", rc, 2);
    QTEST_CHECK_EQ_INT(qcc_macro_identical(m1, m3), 0, "whitespace differs");

    /* Differing length -> not identical. */
    qcc_macro *m4 = obj_macro(&a, &in, "M", ra, 1);
    QTEST_CHECK_EQ_INT(qcc_macro_identical(m1, m4), 0, "length differs");

    /* Object vs function-like -> not identical. */
    qcc_macro *m5 = obj_macro(&a, &in, "M", ra, 2);
    m5->is_function_like = 1;
    QTEST_CHECK_EQ_INT(qcc_macro_identical(m1, m5), 0, "form differs");

    QTEST_CHECK_EQ_INT(qcc_macro_identical(NULL, NULL), 1, "both NULL");
    QTEST_CHECK_EQ_INT(qcc_macro_identical(m1, NULL), 0, "one NULL");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* Growth + deletion: define many, remove half, confirm the rest stay findable
   (the backward-shift deletion must not orphan probe chains). */
static void test_growth_and_deletion(void)
{
    qcc_arena       a;
    qcc_intern      in;
    qcc_macro_table t;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);
    qcc_macro_table_init(&t);

    enum { N = 300 };
    char buf[32];
    qcc_ptok repl[1] = { ident_tok(&in, "z", 0) };
    for (int i = 0; i < N; ++i) {
        snprintf(buf, sizeof(buf), "m%d", i);
        QTEST_CHECK_EQ_INT(qcc_macro_put(&t, obj_macro(&a, &in, buf, repl, 1)),
                           QCC_OK, "put");
    }
    QTEST_CHECK_EQ_UINT(qcc_macro_count(&t), (unsigned)N, "all inserted");
    QTEST_CHECK_TRUE(t.bucket_count > 64); /* grew */

    /* Remove even indices. */
    for (int i = 0; i < N; i += 2) {
        snprintf(buf, sizeof(buf), "m%d", i);
        QTEST_CHECK_EQ_INT(qcc_macro_remove(&t, qcc_intern_str(&in, buf)), 1, "rm");
    }

    int found_ok = 1, gone_ok = 1;
    for (int i = 0; i < N; ++i) {
        snprintf(buf, sizeof(buf), "m%d", i);
        qcc_macro *m = qcc_macro_lookup(&t, qcc_intern_str(&in, buf));
        if ((i % 2 == 1) && m == NULL) {
            found_ok = 0; /* odd survivors must remain findable */
        }
        if ((i % 2 == 0) && m != NULL) {
            gone_ok = 0;  /* evens removed */
        }
    }
    QTEST_CHECK_TRUE(found_ok);
    QTEST_CHECK_TRUE(gone_ok);
    QTEST_CHECK_EQ_UINT(qcc_macro_count(&t), (unsigned)(N / 2), "half remain");

    qcc_macro_table_dispose(&t);
    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

int main(void)
{
    test_put_lookup_remove();
    test_redefine_replaces();
    test_identical();
    test_growth_and_deletion();
    return qtest_report("macro");
}
