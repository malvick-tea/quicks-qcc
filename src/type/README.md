# module: type

**Responsibility:** the representation of C types (ISO C11 §6.2.5) — the thing the
declaration parser builds, semantic analysis queries, `sizeof`/`_Alignof`
measure, and codegen lowers. Owns the single source of truth for object sizes and
alignment on the x86-64 System V LP64 target. Design in
[ADR-0020](../../../Quicks-Meta/docs/adr/0020-qcc-type-representation.md).

**Public interface:** `type/type.h` —
- `qcc_type` + `qcc_type_kind`: a canonical type node, tagged by kind, fields
  valid per kind. The basic arithmetic types are distinct kinds (so `char`,
  `signed char`, and `unsigned char` are three types, §6.2.5 ¶15); derived types
  (pointer, array, function) point at their constituents; struct/union/enum carry
  a tag and a completeness flag.
- `qcc_type_ctx`: owns an `arena` and caches the basic types as singletons.
- constructors: `qcc_type_basic`, `qcc_type_qualified`, `qcc_type_pointer`,
  `qcc_type_array`, `qcc_type_function`, `qcc_type_tagged`.
- queries: `qcc_type_is_{integer,floating,arithmetic,scalar,signed_integer,
  unsigned_integer}`, `qcc_type_equal` (§6.2.7 compatibility), `qcc_type_size` /
  `qcc_type_align`, `qcc_type_kind_name`, and `qcc_type_print` (a readable
  description for diagnostics/tests).

**Model:** one `qcc_type` walked by a single `switch`, the same idiom `ast` and
`token` use. Qualifiers (`const`/`volatile`/`restrict`/`_Atomic`, §6.7.3) are a
bitmask on any type. An unqualified basic type is a singleton, so it compares by
pointer; derived and qualified types are compared structurally by
`qcc_type_equal`. Sizes/alignment are the LP64 values (`int` 4, `long`/pointer 8,
`long double` 16, …); incomplete or sizeless types (void, function, `[]`) report
size 0 for the caller to diagnose.

**Printer form** (`qcc_type_print`): "const int", "pointer to int",
"array[3] of int", "function(int, int) returning int" — readable and unambiguous,
not the inside-out C declarator spelling (which can be added later for user
messages without touching the representation).

**Staged next (ADR-0020):** struct/union/enum start incomplete (tag + flag); the
member/enumerator lists and struct layout land with the struct-definition step of
parser Unit 2 (§6.7), extending this module without disturbing the scalar/derived
core.

**Dependencies:** `arena`, `status`.
