/*
 * Tests for the arena allocator: alignment guarantees, growth across blocks,
 * dedicated over-sized blocks, overflow-checked calloc, memdup/strdup, and the
 * reset/dispose lifecycle. Each case states the property it locks down.
 */
#include "qtest.h"

#include <stdint.h>

#include "arena/arena.h"

/* Returns 1 if `p` is aligned to `align` (a power of two). */
static int is_aligned(const void *p, size_t align)
{
    return ((uintptr_t)p & (uintptr_t)(align - 1)) == 0;
}

/* Basic allocation: non-NULL, writable, default alignment, used grows. */
static void test_basic_alloc(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);
    QTEST_CHECK_EQ_UINT(a.block_size, QCC_ARENA_DEFAULT_BLOCK_SIZE, "default size");
    QTEST_CHECK_EQ_UINT(a.block_count, 0, "lazy: no blocks before first alloc");

    unsigned char *p = (unsigned char *)qcc_arena_alloc(&a, 100, 0);
    QTEST_CHECK_TRUE(p != NULL);
    QTEST_CHECK_TRUE(is_aligned(p, QCC_ARENA_DEFAULT_ALIGN));
    QTEST_CHECK_EQ_UINT(a.block_count, 1, "one block after first alloc");

    for (int i = 0; i < 100; ++i) {
        p[i] = (unsigned char)i; /* Must be fully writable (no overlap/OOB). */
    }
    QTEST_CHECK_EQ_INT(p[42], 42, "written byte reads back");

    qcc_arena_dispose(&a);
    QTEST_CHECK_EQ_UINT(a.block_count, 0, "dispose frees blocks");
    QTEST_CHECK_EQ_UINT(a.block_size, 0, "dispose makes arena inert");
}

/* Distinct allocations never alias, including zero-size ones. */
static void test_distinct_addresses(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);

    void *x = qcc_arena_alloc(&a, 8, 0);
    void *y = qcc_arena_alloc(&a, 8, 0);
    void *z0 = qcc_arena_alloc(&a, 0, 0);  /* zero-size still unique */
    void *z1 = qcc_arena_alloc(&a, 0, 0);

    QTEST_CHECK_TRUE(x != NULL && y != NULL && z0 != NULL && z1 != NULL);
    QTEST_CHECK_TRUE(x != y);
    QTEST_CHECK_TRUE(z0 != z1);
    QTEST_CHECK_TRUE((uintptr_t)y >= (uintptr_t)x + 8); /* no overlap */

    qcc_arena_dispose(&a);
}

/* Explicit power-of-two alignments are honored; bad alignments are rejected. */
static void test_alignment(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);

    /* Force a misaligned cursor (1 byte), then demand 64-byte alignment. */
    (void)qcc_arena_alloc(&a, 1, 1);
    void *p64 = qcc_arena_alloc(&a, 32, 64);
    QTEST_CHECK_TRUE(p64 != NULL);
    QTEST_CHECK_TRUE(is_aligned(p64, 64));

    void *p256 = qcc_arena_alloc(&a, 1, 256);
    QTEST_CHECK_TRUE(p256 != NULL);
    QTEST_CHECK_TRUE(is_aligned(p256, 256));

    /* Non-power-of-two alignment is a programmer error -> NULL. */
    QTEST_CHECK_TRUE(qcc_arena_alloc(&a, 4, 3) == NULL);
    QTEST_CHECK_TRUE(qcc_arena_alloc(&a, 4, 6) == NULL);

    qcc_arena_dispose(&a);
}

/* Many allocations spill into additional blocks; all stay valid and writable. */
static void test_growth_across_blocks(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 256); /* Small blocks so we cross boundaries quickly. */

    enum { N = 200 };
    unsigned char *ptrs[N];
    for (int i = 0; i < N; ++i) {
        ptrs[i] = (unsigned char *)qcc_arena_alloc(&a, 8, 8);
        QTEST_CHECK_TRUE(ptrs[i] != NULL);
        QTEST_CHECK_TRUE(is_aligned(ptrs[i], 8));
        for (int j = 0; j < 8; ++j) {
            ptrs[i][j] = (unsigned char)(i + j);
        }
    }
    QTEST_CHECK_TRUE(a.block_count > 1); /* Genuinely grew. */

    /* Re-read everything: a later block must not have stomped an earlier one. */
    int ok = 1;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (ptrs[i][j] != (unsigned char)(i + j)) {
                ok = 0;
            }
        }
    }
    QTEST_CHECK_TRUE(ok);

    qcc_arena_dispose(&a);
}

/* A request larger than block_size gets its own dedicated block and still
   honors alignment. */
static void test_oversized_allocation(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 128);

    unsigned char *big = (unsigned char *)qcc_arena_alloc(&a, 4096, 16);
    QTEST_CHECK_TRUE(big != NULL);
    QTEST_CHECK_TRUE(is_aligned(big, 16));
    big[0]    = 0xAA;
    big[4095] = 0xBB;
    QTEST_CHECK_EQ_INT(big[0], 0xAA, "first byte");
    QTEST_CHECK_EQ_INT(big[4095], 0xBB, "last byte");
    QTEST_CHECK_TRUE(a.bytes_reserved >= 4096);

    qcc_arena_dispose(&a);
}

/* calloc zeroes and rejects multiplicative overflow. */
static void test_calloc(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);

    unsigned char *z = (unsigned char *)qcc_arena_calloc(&a, 64, 4, 0);
    QTEST_CHECK_TRUE(z != NULL);
    int all_zero = 1;
    for (int i = 0; i < 64 * 4; ++i) {
        if (z[i] != 0) {
            all_zero = 0;
        }
    }
    QTEST_CHECK_TRUE(all_zero);

    /* count * size overflow -> NULL, no allocation. */
    void *bad = qcc_arena_calloc(&a, (SIZE_MAX / 2) + 1, 4, 0);
    QTEST_CHECK_TRUE(bad == NULL);

    qcc_arena_dispose(&a);
}

/* memdup and strdup copy into arena lifetime. */
static void test_dup(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 0);

    const char src[] = {1, 2, 3, 4, 5};
    unsigned char *m = (unsigned char *)qcc_arena_memdup(&a, src, sizeof(src), 1);
    QTEST_CHECK_TRUE(m != NULL && (const void *)m != (const void *)src);
    QTEST_CHECK_EQ_INT(m[0], 1, "memdup[0]");
    QTEST_CHECK_EQ_INT(m[4], 5, "memdup[4]");

    char *s = qcc_arena_strdup(&a, "hello");
    QTEST_CHECK_TRUE(s != NULL);
    QTEST_CHECK_SPAN(s, 5, "hello", "strdup contents");
    QTEST_CHECK_EQ_INT(s[5], '\0', "strdup terminator");

    /* memdup of zero bytes is valid; NULL src is allowed only when size==0. */
    QTEST_CHECK_TRUE(qcc_arena_memdup(&a, NULL, 0, 1) != NULL);
    QTEST_CHECK_TRUE(qcc_arena_memdup(&a, NULL, 4, 1) == NULL);

    qcc_arena_dispose(&a);
}

/* reset frees blocks but keeps the arena configured and reusable. */
static void test_reset_reuse(void)
{
    qcc_arena a;
    qcc_arena_init(&a, 256);
    for (int i = 0; i < 50; ++i) {
        QTEST_CHECK_TRUE(qcc_arena_alloc(&a, 16, 8) != NULL);
    }
    QTEST_CHECK_TRUE(a.block_count >= 1);

    qcc_arena_reset(&a);
    QTEST_CHECK_EQ_UINT(a.block_count, 0, "reset frees blocks");
    QTEST_CHECK_EQ_UINT(a.bytes_used, 0, "reset zeroes used");
    QTEST_CHECK_EQ_UINT(a.block_size, 256, "reset keeps block_size");

    /* Still usable after reset. */
    void *p = qcc_arena_alloc(&a, 16, 8);
    QTEST_CHECK_TRUE(p != NULL);
    QTEST_CHECK_EQ_UINT(a.block_count, 1, "reuse allocates fresh block");

    qcc_arena_dispose(&a);
}

/* NULL-safety on every entry point. */
static void test_null_safety(void)
{
    QTEST_CHECK_TRUE(qcc_arena_alloc(NULL, 8, 0) == NULL);
    QTEST_CHECK_TRUE(qcc_arena_calloc(NULL, 8, 8, 0) == NULL);
    QTEST_CHECK_TRUE(qcc_arena_strdup(NULL, "x") == NULL);
    qcc_arena_init(NULL, 0);   /* must not crash */
    qcc_arena_reset(NULL);     /* must not crash */
    qcc_arena_dispose(NULL);   /* must not crash */
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_basic_alloc();
    test_distinct_addresses();
    test_alignment();
    test_growth_across_blocks();
    test_oversized_allocation();
    test_calloc();
    test_dup();
    test_reset_reuse();
    test_null_safety();
    return qtest_report("arena");
}
