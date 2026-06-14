/*
 * Tests for the type module (ISO C11 §6.2.5, §6.2.7; ADR-0020): basic-type
 * singletons, derived-type construction, qualifiers, classification predicates,
 * structural equality, LP64 sizes/alignment, and the readable printer.
 */
#include "qtest.h"

#include <stdlib.h>
#include <string.h>

#include "type/type.h"

/* Print `t` and compare against `expected`. */
static void chk_print(const qcc_type *t, const char *expected, const char *what)
{
    char  *s   = NULL;
    size_t len = 0;
    QTEST_CHECK_EQ_INT(qcc_type_print(t, &s, &len), QCC_OK, "print ok");
    if (s != NULL) {
        QTEST_CHECK_SPAN(s, len, expected, what);
        free(s);
    }
}

static void test_basics_are_singletons(void)
{
    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);

    const qcc_type *i1 = qcc_type_basic(&ctx, QCC_TYPE_INT);
    const qcc_type *i2 = qcc_type_basic(&ctx, QCC_TYPE_INT);
    QTEST_CHECK_TRUE(i1 == i2); /* same singleton */
    QTEST_CHECK_TRUE(i1 != qcc_type_basic(&ctx, QCC_TYPE_UINT));

    /* char, signed char and unsigned char are three distinct types (§6.2.5). */
    QTEST_CHECK_TRUE(qcc_type_basic(&ctx, QCC_TYPE_CHAR) !=
                     qcc_type_basic(&ctx, QCC_TYPE_SCHAR));
    QTEST_CHECK_TRUE(qcc_type_basic(&ctx, QCC_TYPE_CHAR) !=
                     qcc_type_basic(&ctx, QCC_TYPE_UCHAR));

    QTEST_CHECK_TRUE(qcc_type_basic(&ctx, QCC_TYPE_POINTER) == NULL); /* not basic */

    qcc_type_ctx_dispose(&ctx);
}

static void test_sizes(void)
{
    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);

    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_CHAR)), 1, "char");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_SHORT)), 2, "short");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_INT)), 4, "int");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_LONG)), 8, "long");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_LLONG)), 8, "long long");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_DOUBLE)), 8, "double");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_LDOUBLE)), 16, "long double");

    const qcc_type *intp = qcc_type_pointer(&ctx, qcc_type_basic(&ctx, QCC_TYPE_INT), 0);
    QTEST_CHECK_EQ_UINT(qcc_type_size(intp), 8, "pointer");

    /* int[3] is 12 bytes; aligned as int (4). */
    const qcc_type *arr = qcc_type_array(&ctx, qcc_type_basic(&ctx, QCC_TYPE_INT), 3, 1);
    QTEST_CHECK_EQ_UINT(qcc_type_size(arr), 12, "int[3] size");
    QTEST_CHECK_EQ_UINT(qcc_type_align(arr), 4, "int[3] align");

    /* Incomplete and sizeless types report 0. */
    const qcc_type *incomplete = qcc_type_array(&ctx, qcc_type_basic(&ctx, QCC_TYPE_INT), 0, 0);
    QTEST_CHECK_EQ_UINT(qcc_type_size(incomplete), 0, "int[] size");
    QTEST_CHECK_EQ_UINT(qcc_type_size(qcc_type_basic(&ctx, QCC_TYPE_VOID)), 0, "void");

    qcc_type_ctx_dispose(&ctx);
}

static void test_classification(void)
{
    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);

    QTEST_CHECK_TRUE(qcc_type_is_integer(qcc_type_basic(&ctx, QCC_TYPE_INT)));
    QTEST_CHECK_TRUE(qcc_type_is_integer(qcc_type_basic(&ctx, QCC_TYPE_BOOL)));
    QTEST_CHECK_TRUE(!qcc_type_is_integer(qcc_type_basic(&ctx, QCC_TYPE_DOUBLE)));
    QTEST_CHECK_TRUE(qcc_type_is_floating(qcc_type_basic(&ctx, QCC_TYPE_DOUBLE)));
    QTEST_CHECK_TRUE(qcc_type_is_arithmetic(qcc_type_basic(&ctx, QCC_TYPE_FLOAT)));

    const qcc_type *p = qcc_type_pointer(&ctx, qcc_type_basic(&ctx, QCC_TYPE_INT), 0);
    QTEST_CHECK_TRUE(qcc_type_is_scalar(p));
    QTEST_CHECK_TRUE(!qcc_type_is_arithmetic(p));

    QTEST_CHECK_TRUE(qcc_type_is_signed_integer(qcc_type_basic(&ctx, QCC_TYPE_INT)));
    QTEST_CHECK_TRUE(qcc_type_is_unsigned_integer(qcc_type_basic(&ctx, QCC_TYPE_UINT)));
    QTEST_CHECK_TRUE(qcc_type_is_unsigned_integer(qcc_type_basic(&ctx, QCC_TYPE_BOOL)));
    QTEST_CHECK_TRUE(qcc_type_is_signed_integer(qcc_type_basic(&ctx, QCC_TYPE_CHAR)));

    qcc_type_ctx_dispose(&ctx);
}

static void test_equality(void)
{
    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);
    const qcc_type *i = qcc_type_basic(&ctx, QCC_TYPE_INT);

    /* Two independently built int* compare equal (structural, §6.2.7). */
    QTEST_CHECK_TRUE(qcc_type_equal(qcc_type_pointer(&ctx, i, 0),
                                    qcc_type_pointer(&ctx, i, 0)));
    /* int* != char*. */
    QTEST_CHECK_TRUE(!qcc_type_equal(
        qcc_type_pointer(&ctx, i, 0),
        qcc_type_pointer(&ctx, qcc_type_basic(&ctx, QCC_TYPE_CHAR), 0)));

    /* const int != int (qualifiers differ). */
    const qcc_type *ci = qcc_type_qualified(&ctx, i, QCC_QUAL_CONST);
    QTEST_CHECK_TRUE(!qcc_type_equal(ci, i));
    QTEST_CHECK_TRUE(qcc_type_qualified(&ctx, i, 0) == i); /* no-op qualify */
    /* Re-qualifying with the same bit is idempotent in value. */
    QTEST_CHECK_TRUE(qcc_type_equal(ci, qcc_type_qualified(&ctx, i, QCC_QUAL_CONST)));

    /* A known bound is compatible with an unknown one but not a different one. */
    const qcc_type *a3  = qcc_type_array(&ctx, i, 3, 1);
    const qcc_type *a3b = qcc_type_array(&ctx, i, 3, 1);
    const qcc_type *a5  = qcc_type_array(&ctx, i, 5, 1);
    const qcc_type *ax  = qcc_type_array(&ctx, i, 0, 0);
    QTEST_CHECK_TRUE(qcc_type_equal(a3, a3b));
    QTEST_CHECK_TRUE(!qcc_type_equal(a3, a5));
    QTEST_CHECK_TRUE(qcc_type_equal(a3, ax));

    /* Functions compare by return, params, and variadic flag. */
    const qcc_type *params[1] = { i };
    const qcc_type *f1 = qcc_type_function(&ctx, i, params, 1, 0);
    const qcc_type *f2 = qcc_type_function(&ctx, i, params, 1, 0);
    const qcc_type *fv = qcc_type_function(&ctx, i, params, 1, 1);
    QTEST_CHECK_TRUE(qcc_type_equal(f1, f2));
    QTEST_CHECK_TRUE(!qcc_type_equal(f1, fv));

    /* Tagged types match by tag name. */
    const qcc_type *s1 = qcc_type_tagged(&ctx, QCC_TYPE_STRUCT, "foo", 3, 0);
    const qcc_type *s2 = qcc_type_tagged(&ctx, QCC_TYPE_STRUCT, "foo", 3, 1);
    const qcc_type *s3 = qcc_type_tagged(&ctx, QCC_TYPE_STRUCT, "bar", 3, 1);
    QTEST_CHECK_TRUE(qcc_type_equal(s1, s2)); /* same tag, completeness differs */
    QTEST_CHECK_TRUE(!qcc_type_equal(s1, s3));

    qcc_type_ctx_dispose(&ctx);
}

static void test_print(void)
{
    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);
    const qcc_type *i = qcc_type_basic(&ctx, QCC_TYPE_INT);

    chk_print(i, "int", "int");
    chk_print(qcc_type_basic(&ctx, QCC_TYPE_ULONG), "unsigned long", "ulong");
    chk_print(qcc_type_qualified(&ctx, i, QCC_QUAL_CONST), "const int", "const int");
    chk_print(qcc_type_pointer(&ctx, i, 0), "pointer to int", "int*");
    /* int *const: the pointer itself is const. */
    chk_print(qcc_type_pointer(&ctx, i, QCC_QUAL_CONST), "const pointer to int",
              "int *const");
    /* pointer to const int. */
    chk_print(qcc_type_pointer(&ctx, qcc_type_qualified(&ctx, i, QCC_QUAL_CONST), 0),
              "pointer to const int", "const int*");
    chk_print(qcc_type_array(&ctx, i, 3, 1), "array[3] of int", "int[3]");
    chk_print(qcc_type_array(&ctx, i, 0, 0), "array[] of int", "int[]");

    const qcc_type *params[2] = { i, qcc_type_basic(&ctx, QCC_TYPE_CHAR) };
    chk_print(qcc_type_function(&ctx, i, params, 2, 0),
              "function(int, char) returning int", "function");
    chk_print(qcc_type_function(&ctx, qcc_type_basic(&ctx, QCC_TYPE_VOID),
                                params, 1, 1),
              "function(int, ...) returning void", "variadic function");

    chk_print(qcc_type_tagged(&ctx, QCC_TYPE_STRUCT, "node", 4, 0),
              "struct node", "struct");

    /* A pointer to a function: pointer to function(int) returning int. */
    const qcc_type *fp = qcc_type_pointer(
        &ctx, qcc_type_function(&ctx, i, params, 1, 0), 0);
    chk_print(fp, "pointer to function(int) returning int", "function pointer");

    qcc_type_ctx_dispose(&ctx);
}

static void test_invalid_args(void)
{
    QTEST_CHECK_EQ_INT(qcc_type_ctx_init(NULL), QCC_ERR_INVALID_ARGUMENT, "null init");
    qcc_type_ctx_dispose(NULL);

    qcc_type_ctx ctx;
    qcc_type_ctx_init(&ctx);
    QTEST_CHECK_TRUE(qcc_type_tagged(&ctx, QCC_TYPE_INT, "x", 1, 0) == NULL);
    QTEST_CHECK_TRUE(qcc_type_equal(NULL, NULL));
    QTEST_CHECK_TRUE(!qcc_type_equal(NULL, qcc_type_basic(&ctx, QCC_TYPE_INT)));
    char  *s   = NULL;
    size_t len = 0;
    QTEST_CHECK_EQ_INT(qcc_type_print(NULL, &s, &len), QCC_ERR_INVALID_ARGUMENT, "null print");
    qcc_type_ctx_dispose(&ctx);
}

int main(void)
{
    test_basics_are_singletons();
    test_sizes();
    test_classification();
    test_equality();
    test_print();
    test_invalid_args();
    return qtest_report("type");
}
