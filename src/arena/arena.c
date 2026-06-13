/*
 * qcc — arena (region) allocator: implementation.
 *
 * See arena.h for the contract and rationale. The arena is a singly linked
 * list of blocks with `head` pointing at the newest. Each block carries its
 * own capacity and a bump cursor (`used`); the usable bytes follow the block
 * header in the same malloc allocation, so one free() releases header + data.
 *
 * Alignment is computed from the real address of each allocation, not assumed
 * from the header size, because the strictest scalar alignment (max_align_t)
 * can exceed the header's natural alignment (C11 §6.2.8). All size/alignment
 * arithmetic is overflow-checked at the trust boundary (coding-standard.md §7).
 */
#include "arena/arena.h"

#include <stdlib.h>
#include <string.h>

struct qcc_arena_block {
    qcc_arena_block *next;      /* Previous (older) block, or NULL.            */
    size_t           capacity;  /* Usable bytes that follow this header.       */
    size_t           used;      /* Bytes consumed from the data area.          */
    /* `capacity` bytes of payload follow immediately (see data_of below).     */
};

/* Private helpers. */

/* A valid alignment is a nonzero power of two (C11 §6.2.8). */
static int is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* Start of a block's usable payload, just past its header. */
static unsigned char *data_of(qcc_arena_block *block)
{
    return (unsigned char *)(block + 1);
}

/*
 * Round `value` up to the next multiple of `align` (a power of two), reporting
 * overflow rather than wrapping. Returns 1 and writes *out on success, 0 if the
 * rounding would exceed UINTPTR_MAX.
 */
static int align_up_uintptr(uintptr_t value, size_t align, uintptr_t *out)
{
    uintptr_t mask = (uintptr_t)align - 1u;
    if (value > UINTPTR_MAX - mask) {
        return 0; /* value + mask would wrap. */
    }
    *out = (value + mask) & ~mask;
    return 1;
}

/*
 * Try to satisfy a `bump`-byte, `align`-aligned request from `block` without
 * growing. On success advances the cursor, adds to *bytes_used, and returns the
 * aligned pointer; on "does not fit" returns NULL with the block unchanged.
 */
static void *block_try_alloc(qcc_arena_block *block, size_t bump, size_t align,
                             size_t *bytes_used)
{
    uintptr_t base = (uintptr_t)data_of(block) + (uintptr_t)block->used;
    uintptr_t aligned;
    if (!align_up_uintptr(base, align, &aligned)) {
        return NULL;
    }

    size_t pad   = (size_t)(aligned - base);
    size_t avail = block->capacity - block->used; /* used <= capacity invariant */
    if (pad > avail || bump > avail - pad) {
        return NULL;
    }

    block->used += pad + bump;
    *bytes_used += pad + bump;
    return (void *)aligned;
}

/*
 * Allocate and push a new block large enough to hold a `bump`-byte request at
 * `align` (worst-case padding is align-1 bytes because the payload start is not
 * assumed aligned). Ordinary requests get a full block_size block so later
 * allocations share it; an over-sized request gets an exact-fit dedicated
 * block. Returns the new head, or NULL on OOM/overflow.
 */
static qcc_arena_block *grow(qcc_arena *arena, size_t bump, size_t align)
{
    size_t worst_pad = align - 1u; /* align is a validated power of two >= 1. */
    if (bump > SIZE_MAX - worst_pad) {
        return NULL; /* bump + worst_pad would overflow. */
    }
    size_t needed = bump + worst_pad;

    size_t capacity = (needed > arena->block_size) ? needed : arena->block_size;
    if (capacity > SIZE_MAX - sizeof(qcc_arena_block)) {
        return NULL; /* header + capacity would overflow the malloc argument. */
    }

    qcc_arena_block *block =
        (qcc_arena_block *)malloc(sizeof(qcc_arena_block) + capacity);
    if (block == NULL) {
        return NULL;
    }
    block->next     = arena->head;
    block->capacity = capacity;
    block->used     = 0;

    arena->head            = block;
    arena->bytes_reserved += capacity;
    arena->block_count    += 1;
    return block;
}

/* Public interface. */

void qcc_arena_init(qcc_arena *arena, size_t block_size)
{
    if (arena == NULL) {
        return;
    }
    arena->head           = NULL;
    arena->block_size     = (block_size != 0) ? block_size
                                              : QCC_ARENA_DEFAULT_BLOCK_SIZE;
    arena->bytes_used     = 0;
    arena->bytes_reserved = 0;
    arena->block_count    = 0;
}

void *qcc_arena_alloc(qcc_arena *arena, size_t size, size_t align)
{
    if (arena == NULL) {
        return NULL;
    }
    if (align == 0) {
        align = QCC_ARENA_DEFAULT_ALIGN;
    }
    if (!is_power_of_two(align)) {
        return NULL; /* Programmer error: alignment must be a power of two. */
    }

    /* A zero-size request still yields a distinct, bumped address (arena.h):
       reserve one byte so two zero-size allocations never alias. */
    size_t bump = (size != 0) ? size : 1u;

    if (arena->head != NULL) {
        void *p = block_try_alloc(arena->head, bump, align, &arena->bytes_used);
        if (p != NULL) {
            return p;
        }
    }

    /* Current block (if any) is full for this request; grow and serve from the
       fresh block, which is sized to guarantee the request fits. */
    qcc_arena_block *block = grow(arena, bump, align);
    if (block == NULL) {
        return NULL;
    }
    return block_try_alloc(block, bump, align, &arena->bytes_used);
}

void *qcc_arena_calloc(qcc_arena *arena, size_t count, size_t size, size_t align)
{
    size_t total;
    if (count == 0 || size == 0) {
        total = 0;
    } else if (size > SIZE_MAX / count) {
        return NULL; /* count * size would overflow. */
    } else {
        total = count * size;
    }

    void *p = qcc_arena_alloc(arena, total, align);
    if (p != NULL && total != 0) {
        memset(p, 0, total);
    }
    return p;
}

void *qcc_arena_memdup(qcc_arena *arena, const void *src, size_t size, size_t align)
{
    if (src == NULL && size != 0) {
        return NULL;
    }
    void *p = qcc_arena_alloc(arena, size, align);
    if (p != NULL && size != 0) {
        memcpy(p, src, size);
    }
    return p;
}

char *qcc_arena_strdup(qcc_arena *arena, const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t size = strlen(s) + 1u; /* Include the terminating NUL. */
    return (char *)qcc_arena_memdup(arena, s, size, 1u);
}

void qcc_arena_reset(qcc_arena *arena)
{
    if (arena == NULL) {
        return;
    }
    qcc_arena_block *block = arena->head;
    while (block != NULL) {
        qcc_arena_block *prev = block->next;
        free(block);
        block = prev;
    }
    arena->head           = NULL;
    arena->bytes_used     = 0;
    arena->bytes_reserved = 0;
    arena->block_count    = 0;
    /* block_size is intentionally preserved so the arena stays configured. */
}

void qcc_arena_dispose(qcc_arena *arena)
{
    if (arena == NULL) {
        return;
    }
    qcc_arena_reset(arena);
    arena->block_size = 0; /* Inert until re-init (distinguishes from reset). */
}
