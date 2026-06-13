/*
 * qcc — arena (region) allocator
 *
 * Responsibility
 * Hand out many small, same-lifetime allocations cheaply and free them all in
 * one operation. A compiler front end allocates a great number of tiny, long-
 * lived objects — preprocessing tokens, macro records, hide-set cells, AST
 * nodes — that all die together at the end of a translation unit. An arena
 * makes each allocation a pointer bump and teardown a single pass over a short
 * list of blocks, which is both faster than per-object malloc/free and far
 * safer on error paths: a failing stage destroys its arena and leaks nothing,
 * with no hand-written unwinding (Quicks-Meta/docs/standards/error-handling.md,
 * "prefer arena/region allocation … release everything at once").
 *
 * Design (and why)
 *   - A chain of heap blocks. Allocation bumps a cursor in the newest block;
 *     when a request does not fit, a fresh block is pushed. There is no
 *     per-object free — that is the whole point — only whole-arena reset/dispose.
 *   - Every allocation is aligned explicitly to the caller's requested power-of-
 *     two alignment, computed from the real address, so we never rely on the
 *     block header's size being a multiple of max_align_t (it need not be —
 *     C11 §6.2.8 alignment requirements).
 *   - An over-sized request (larger than the default block) gets its own exact
 *     block, so one big allocation never wastes a whole default block and never
 *     forces the default block size up.
 *
 * Ownership
 *   Memory returned by the arena is owned by the arena: never free() it. It
 *   stays valid until qcc_arena_reset() or qcc_arena_dispose(). The arena uses
 *   the seed CRT allocator for now (ADR-0009); it is replaced wholesale when
 *   qlibc lands, with no change to this interface.
 *
 * Standard: ISO/IEC 9899 (C11), portable subset (ADR-0006). Uses <stdint.h>
 * (uintptr_t for alignment arithmetic, intmax_t/uintmax_t) and <stddef.h>
 * (size_t).
 */
#ifndef QCC_ARENA_ARENA_H
#define QCC_ARENA_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "status/status.h"

/*
 * The strictest fundamental alignment, derived portably. C11 provides
 * max_align_t in <stddef.h>, but some seed C libraries omit it (MSVC's does),
 * so we take the alignment of a union of the widest standard types instead —
 * any scalar the compiler supports aligns no more strictly than the widest of
 * these. _Alignof is C11 §6.5.3.4. This is the alignment qcc_arena_alloc uses
 * when the caller passes align == 0.
 */
typedef union qcc_max_align {
    long long      widest_integer;
    long double    widest_float;
    void          *object_pointer;
    void         (*function_pointer)(void);
    intmax_t       max_signed;
    uintmax_t      max_unsigned;
} qcc_max_align;

/*
 * Default capacity of a freshly grown block, in bytes. 64 KiB amortizes the
 * malloc cost over many small allocations while staying small enough that an
 * arena holding only a few tokens does not reserve megabytes. A request larger
 * than this gets its own exact-sized block instead (see qcc_arena_alloc).
 */
#define QCC_ARENA_DEFAULT_BLOCK_SIZE ((size_t)65536u)

/*
 * Default alignment when the caller passes align == 0: the strictest alignment
 * any scalar type requires (C11 §6.2.8, _Alignof(max_align_t)). Using it makes
 * qcc_arena_alloc(a, n, 0) behave like malloc(n) with respect to alignment.
 */
#define QCC_ARENA_DEFAULT_ALIGN (_Alignof(qcc_max_align))

/*
 * One arena. Treat the fields as private; use the functions below. The struct
 * may live on the stack — only the block chain it points at is heap-owned.
 */
typedef struct qcc_arena_block qcc_arena_block; /* Opaque; defined in arena.c. */

typedef struct qcc_arena {
    qcc_arena_block *head;        /* Newest block; the bump cursor lives here.   */
    size_t           block_size;  /* Capacity used for ordinary new blocks.      */
    size_t           bytes_used;  /* Total bytes handed out (incl. alignment pad)*/
    size_t           bytes_reserved; /* Total usable capacity across all blocks. */
    size_t           block_count; /* Number of live blocks (for tests/stats).    */
} qcc_arena;

/*
 * Initialize an empty arena. `block_size` is the capacity of ordinary blocks;
 * pass 0 for QCC_ARENA_DEFAULT_BLOCK_SIZE. No memory is allocated until the
 * first qcc_arena_alloc (lazy: an unused arena costs nothing). Always succeeds.
 */
void qcc_arena_init(qcc_arena *arena, size_t block_size);

/*
 * Allocate `size` bytes aligned to `align` (a power of two; 0 means
 * QCC_ARENA_DEFAULT_ALIGN). Returns a pointer owned by the arena, or NULL on
 * out-of-memory or on an invalid request (align not a power of two, or size/
 * alignment arithmetic would overflow). size == 0 returns a valid, uniquely
 * bumped, suitably aligned pointer (never NULL for a zero request that fits),
 * mirroring the convention that distinct objects have distinct addresses.
 *
 * The bytes are uninitialized; use qcc_arena_calloc when you need them zeroed.
 */
void *qcc_arena_alloc(qcc_arena *arena, size_t size, size_t align);

/*
 * Like qcc_arena_alloc for `count * size` bytes, but the result is zeroed and
 * the multiplication is overflow-checked (returns NULL if count * size would
 * wrap). Use for arrays whose element count comes from untrusted input.
 */
void *qcc_arena_calloc(qcc_arena *arena, size_t count, size_t size, size_t align);

/*
 * Copy `size` bytes from `src` into freshly arena-allocated, `align`-aligned
 * memory and return it (NULL on OOM/invalid request). `src` may be NULL only if
 * size == 0. Used to move a transient buffer (e.g. a built-up token list) into
 * arena lifetime.
 */
void *qcc_arena_memdup(qcc_arena *arena, const void *src, size_t size, size_t align);

/*
 * Copy a NUL-terminated string into the arena (including its terminator) and
 * return it, or NULL on OOM. `s` must be non-NULL. Convenience over
 * qcc_arena_memdup for the common string case (default alignment is fine for
 * char data).
 */
char *qcc_arena_strdup(qcc_arena *arena, const char *s);

/*
 * Release every block back to the allocator but keep the arena usable: the
 * configured block_size is retained, counters reset to zero, and the next
 * allocation lazily grows a new block. Pointers previously returned are invalid
 * after this call. Use to recycle one arena across translation units.
 */
void qcc_arena_reset(qcc_arena *arena);

/*
 * Release every block and zero the arena. After dispose the arena is inert
 * (as if memset to 0); call qcc_arena_init again to reuse it. Idempotent and
 * NULL-safe.
 */
void qcc_arena_dispose(qcc_arena *arena);

#endif /* QCC_ARENA_ARENA_H */
