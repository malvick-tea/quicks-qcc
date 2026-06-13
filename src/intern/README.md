# module: intern

**Responsibility:** map each distinct byte span to a single stable,
NUL-terminated copy, so identical spans become the *same pointer*. This turns
the preprocessor's hot operations — macro name lookup, hide-set membership, and
the "identical macro redefinition is allowed" check (ISO C11 §6.10.3 ¶2) — into
pointer comparisons instead of repeated `memcmp`/re-lexing
([ADR-0014](../../../Quicks-Meta/docs/adr/0014-qcc-preprocessor-design.md)).

**Public interface:** `intern/intern.h` — `qcc_intern`, `qcc_intern_init`,
`qcc_intern_dispose`, `qcc_intern_bytes`, `qcc_intern_str`, `qcc_intern_count`.

**Design:** an open-addressing hash table (linear probing, power-of-two
capacity) keyed by content, hashed with 64-bit FNV-1a (the same function qas's
symbol table uses). Open addressing keeps the table in one contiguous,
cache-friendly array. Interned bytes are copied into a caller-provided `arena`,
so there is no second lifetime to track — every interned string lives exactly as
long as that arena. Keys are `(bytes, length)` pairs (interior NULs are fine);
the stored copy is additionally NUL-terminated for C-string callers. The table
grows and rehashes at load factor 3/4, reusing each slot's cached hash.

**Key invariants:** equal spans return identical pointers; returned pointers are
`const` and immutable, valid until the arena is reset/disposed; the table always
keeps at least one empty slot so probing terminates; `dispose` frees only the
bucket array, never the interned bytes (the arena owns those).

**Dependencies:** `arena` (stores interned bytes), `status` (result codes).
