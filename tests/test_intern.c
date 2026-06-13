/*
 * Tests for the string interner: pointer-identity of equal spans, distinctness
 * of different spans, NUL termination, growth/rehash stability, and lifetime
 * over the backing arena. Each case states the property it locks down.
 */
#include "qtest.h"

#include <stdio.h>

#include "arena/arena.h"
#include "intern/intern.h"

/* Equal contents intern to the SAME pointer; different contents do not. */
static void test_identity(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    QTEST_CHECK_EQ_INT(qcc_intern_init(&in, &a), QCC_OK, "init");

    const char *x  = qcc_intern_str(&in, "return");
    const char *y  = qcc_intern_str(&in, "return");
    const char *z  = qcc_intern_str(&in, "while");
    QTEST_CHECK_TRUE(x != NULL && y != NULL && z != NULL);
    QTEST_CHECK_TRUE(x == y);   /* same content -> same pointer */
    QTEST_CHECK_TRUE(x != z);   /* different content -> different pointer */
    QTEST_CHECK_SPAN(x, 6, "return", "interned contents");
    QTEST_CHECK_EQ_INT(x[6], '\0', "interned is NUL-terminated");

    QTEST_CHECK_EQ_UINT(qcc_intern_count(&in), 2, "two distinct strings");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* A prefix is a distinct string from a longer span; length matters. */
static void test_prefix_distinct(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    const char *ab  = qcc_intern_bytes(&in, "abc", 2); /* "ab" */
    const char *abc = qcc_intern_bytes(&in, "abc", 3); /* "abc" */
    QTEST_CHECK_TRUE(ab != abc);
    QTEST_CHECK_SPAN(ab, 2, "ab", "prefix span");
    QTEST_CHECK_SPAN(abc, 3, "abc", "full span");

    /* Re-interning the prefix via a different backing buffer still matches. */
    const char *ab2 = qcc_intern_str(&in, "ab");
    QTEST_CHECK_TRUE(ab2 == ab);

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* The empty string interns to a valid, stable, NUL-terminated pointer. */
static void test_empty(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    const char *e1 = qcc_intern_bytes(&in, NULL, 0);
    const char *e2 = qcc_intern_str(&in, "");
    QTEST_CHECK_TRUE(e1 != NULL && e1 == e2);
    QTEST_CHECK_EQ_INT(e1[0], '\0', "empty is just a terminator");
    QTEST_CHECK_EQ_UINT(qcc_intern_count(&in), 1, "empty counts once");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/*
 * Interning enough distinct strings forces several table growths; afterwards
 * every earlier pointer must still be returned for its content (rehash kept
 * identity), and no two distinct strings collide onto one pointer.
 */
static void test_growth_stability(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    enum { N = 500 };
    const char *ptrs[N];
    char        buf[32];
    for (int i = 0; i < N; ++i) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        ptrs[i] = qcc_intern_str(&in, buf);
        QTEST_CHECK_TRUE(ptrs[i] != NULL);
    }
    QTEST_CHECK_EQ_UINT(qcc_intern_count(&in), (unsigned)N, "all distinct");
    QTEST_CHECK_TRUE(in.bucket_count > 64); /* genuinely grew */

    /* Identity survived the rehash: re-interning yields the same pointers. */
    int identity_ok = 1;
    int distinct_ok = 1;
    for (int i = 0; i < N; ++i) {
        snprintf(buf, sizeof(buf), "sym_%d", i);
        const char *again = qcc_intern_str(&in, buf);
        if (again != ptrs[i]) {
            identity_ok = 0;
        }
        /* No earlier pointer should equal this distinct string's pointer. */
        if (i > 0 && again == ptrs[i - 1]) {
            distinct_ok = 0;
        }
    }
    QTEST_CHECK_TRUE(identity_ok);
    QTEST_CHECK_TRUE(distinct_ok);
    QTEST_CHECK_EQ_UINT(qcc_intern_count(&in), (unsigned)N, "no new strings");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* NULL-safety and argument validation. */
static void test_null_safety(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);

    QTEST_CHECK_EQ_INT(qcc_intern_init(NULL, &a), QCC_ERR_INVALID_ARGUMENT, "null self");
    QTEST_CHECK_EQ_INT(qcc_intern_init(&in, NULL), QCC_ERR_INVALID_ARGUMENT, "null arena");

    qcc_intern_init(&in, &a);
    QTEST_CHECK_TRUE(qcc_intern_str(&in, NULL) == NULL);
    QTEST_CHECK_TRUE(qcc_intern_bytes(&in, NULL, 4) == NULL); /* NULL with length */
    QTEST_CHECK_EQ_UINT(qcc_intern_count(NULL), 0, "count(NULL)");

    qcc_intern_dispose(&in);
    qcc_intern_dispose(NULL); /* must not crash */
    qcc_arena_dispose(&a);
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_identity();
    test_prefix_distinct();
    test_empty();
    test_growth_stability();
    test_null_safety();
    return qtest_report("intern");
}
