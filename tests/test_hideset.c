/*
 * Tests for preprocessor hide sets (ISO C11 §6.10.3.4): membership, the add
 * (cons) operation with de-duplication and tail sharing, and set union /
 * intersection. Names are interned so the set compares by pointer identity, so
 * the tests intern their names first.
 *
 * This suite includes the pp module's own internal header — a module verifying
 * its internals is legitimate (coding-standard.md §8 requires unit tests, and
 * the hide-set algebra is too subtle to leave to integration tests only). The
 * "no cross-module internal access" rule (ADR-0008) targets production modules,
 * not a module's own tests.
 */
#include "qtest.h"

#include "arena/arena.h"
#include "intern/intern.h"
#include "pp/internal/hideset.h"

/* Membership and add: adding a present name is a no-op; tails are shared. */
static void test_add_and_contains(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    const char *foo = qcc_intern_str(&in, "foo");
    const char *bar = qcc_intern_str(&in, "bar");
    const char *baz = qcc_intern_str(&in, "baz");

    const qcc_hideset *empty = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_contains(empty, foo), 0, "empty has nothing");
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(empty), 0, "empty size 0");

    const qcc_hideset *s1 = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_add(&a, empty, foo, &s1), QCC_OK, "add foo");
    QTEST_CHECK_TRUE(qcc_hideset_contains(s1, foo));
    QTEST_CHECK_TRUE(!qcc_hideset_contains(s1, bar));
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(s1), 1, "size 1");

    const qcc_hideset *s2 = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_add(&a, s1, bar, &s2), QCC_OK, "add bar");
    QTEST_CHECK_TRUE(qcc_hideset_contains(s2, foo) && qcc_hideset_contains(s2, bar));
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(s2), 2, "size 2");
    /* s2 shares s1's tail: s1 is unchanged (immutability). */
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(s1), 1, "s1 unchanged");

    /* Adding a name already present returns the same set, no growth. */
    const qcc_hideset *s3 = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_add(&a, s2, foo, &s3), QCC_OK, "re-add foo");
    QTEST_CHECK_TRUE(s3 == s2);
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(s3), 2, "no duplicate");

    QTEST_CHECK_TRUE(!qcc_hideset_contains(s2, baz));

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* Union has every name from either set, with no duplicates. */
static void test_union(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    const char *x = qcc_intern_str(&in, "x");
    const char *y = qcc_intern_str(&in, "y");
    const char *z = qcc_intern_str(&in, "z");

    const qcc_hideset *a1 = NULL, *a2 = NULL; /* {x, y} */
    qcc_hideset_add(&a, NULL, x, &a1);
    qcc_hideset_add(&a, a1, y, &a2);

    const qcc_hideset *b1 = NULL, *b2 = NULL; /* {y, z} */
    qcc_hideset_add(&a, NULL, y, &b1);
    qcc_hideset_add(&a, b1, z, &b2);

    const qcc_hideset *u = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_union(&a, a2, b2, &u), QCC_OK, "union");
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(u), 3, "x,y,z once each");
    QTEST_CHECK_TRUE(qcc_hideset_contains(u, x));
    QTEST_CHECK_TRUE(qcc_hideset_contains(u, y));
    QTEST_CHECK_TRUE(qcc_hideset_contains(u, z));

    /* Union with the empty set is the original. */
    const qcc_hideset *u2 = NULL;
    qcc_hideset_union(&a, a2, NULL, &u2);
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(u2), 2, "union with empty");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* Intersection has only names in both sets. */
static void test_intersect(void)
{
    qcc_arena  a;
    qcc_intern in;
    qcc_arena_init(&a, 0);
    qcc_intern_init(&in, &a);

    const char *x = qcc_intern_str(&in, "x");
    const char *y = qcc_intern_str(&in, "y");
    const char *z = qcc_intern_str(&in, "z");

    const qcc_hideset *a1 = NULL, *a2 = NULL; /* {x, y} */
    qcc_hideset_add(&a, NULL, x, &a1);
    qcc_hideset_add(&a, a1, y, &a2);

    const qcc_hideset *b1 = NULL, *b2 = NULL; /* {y, z} */
    qcc_hideset_add(&a, NULL, y, &b1);
    qcc_hideset_add(&a, b1, z, &b2);

    const qcc_hideset *inter = NULL;
    QTEST_CHECK_EQ_INT(qcc_hideset_intersect(&a, a2, b2, &inter), QCC_OK, "intersect");
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(inter), 1, "only y");
    QTEST_CHECK_TRUE(qcc_hideset_contains(inter, y));
    QTEST_CHECK_TRUE(!qcc_hideset_contains(inter, x));
    QTEST_CHECK_TRUE(!qcc_hideset_contains(inter, z));

    /* Intersection with the empty set is empty. */
    const qcc_hideset *inter2 = NULL;
    qcc_hideset_intersect(&a, a2, NULL, &inter2);
    QTEST_CHECK_EQ_UINT(qcc_hideset_size(inter2), 0, "intersect empty");

    qcc_intern_dispose(&in);
    qcc_arena_dispose(&a);
}

/* Argument validation. */
static void test_invalid_args(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);
    const qcc_hideset *out = NULL;
    char dummy = 'd';

    QTEST_CHECK_EQ_INT(qcc_hideset_add(NULL, NULL, &dummy, &out),
                       QCC_ERR_INVALID_ARGUMENT, "null arena");
    QTEST_CHECK_EQ_INT(qcc_hideset_add(&a, NULL, NULL, &out),
                       QCC_ERR_INVALID_ARGUMENT, "null name");
    QTEST_CHECK_EQ_INT(qcc_hideset_add(&a, NULL, &dummy, NULL),
                       QCC_ERR_INVALID_ARGUMENT, "null out");

    qcc_arena_dispose(&a);
}

int main(void)
{
    test_add_and_contains();
    test_union();
    test_intersect();
    test_invalid_args();
    return qtest_report("hideset");
}
