# module: arena

**Responsibility:** a region (bump) allocator for the many small, same-lifetime
allocations the compiler front end makes (preprocessing tokens, macro records,
hide-set cells, AST nodes). Allocation is a pointer bump; there is no per-object
free — the whole arena is reset or disposed at once. This is faster than
per-object `malloc`/`free` and makes error paths leak-free by construction
(destroy the arena), as required by
[error-handling.md](../../../Quicks-Meta/docs/standards/error-handling.md).

**Public interface:** `arena/arena.h` — `qcc_arena`, `qcc_arena_init`,
`qcc_arena_alloc`, `qcc_arena_calloc`, `qcc_arena_memdup`, `qcc_arena_strdup`,
`qcc_arena_reset`, `qcc_arena_dispose`, and the `QCC_ARENA_DEFAULT_BLOCK_SIZE` /
`QCC_ARENA_DEFAULT_ALIGN` constants.

**Design:** a singly linked list of heap blocks; the newest block holds the bump
cursor. A request that does not fit the current block grows a new one — an
ordinary request gets a full `block_size` block (shared by later allocations);
an over-sized request gets its own exact-fit block, so a single big allocation
neither wastes a default block nor inflates the default size. Each allocation is
aligned from its real address (never assuming the block header is `max_align_t`-
aligned, C11 §6.2.8), and all size/alignment arithmetic is overflow-checked.

**Key invariants:** memory returned is owned by the arena and valid until
`qcc_arena_reset`/`qcc_arena_dispose` — never `free()` it; `used <= capacity` in
every block; `reset` keeps the arena configured and reusable, `dispose` leaves
it inert (re-`init` to reuse); a zero-size request returns a distinct, suitably
aligned, non-NULL pointer.

**Seed dependency:** uses the CRT allocator (`malloc`/`free`) for now
([ADR-0009](../../../Quicks-Meta/docs/adr/0009-standard-library-seed-crt-then-qlibc.md));
replaced by qlibc with no interface change.

**Dependencies:** `status` (shared result vocabulary; this module reports
failure as a NULL pointer rather than a status, since allocation has exactly one
failure mode).
