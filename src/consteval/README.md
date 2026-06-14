# module: consteval

**Responsibility:** evaluate an *integer constant expression* (ISO C11 §6.6) over
a parsed `qcc_expr`, with conforming integer promotions (§6.3.1.1) and usual
arithmetic conversions (§6.3.1.8) on the x86-64 System V LP64 target. Design in
[ADR-0025](../../../Quicks-Meta/docs/adr/0025-qcc-integer-constant-expression-evaluator.md).

**Public interface:** `consteval/consteval.h` —
- `qcc_const_value` (`{ uint64_t value; qcc_int_type type; }`): the result. `value`
  is the two's-complement bit pattern reduced to the result type's width
  (sign-extended to 64 bits if signed, zero-extended if unsigned), so
  `(int64_t)value` is the signed interpretation; `type` is the LP64 integer type.
- `qcc_eval_const_int(e, out)`: QCC_OK + `*out` on success; `QCC_ERR_TYPE` if `e`
  is not an integer constant expression this module evaluates;
  `QCC_ERR_INVALID_ARGUMENT` on NULL.

**What it evaluates:** integer and character constants; the unary `+ - ~ !`; the
binary `+ - * / % << >> < > <= >= == != & ^ | && ||`; the conditional `?:`; a cast
to an integer type; and `sizeof`/`_Alignof` of a *type-name* (folded through
`qcc_type_size`/`qcc_type_align`). `&&`, `||`, and `?:` short-circuit, so an
unevaluated operand need not be constant (§6.6 ¶3) — `1 || (1/0)` is `1`.

**What it rejects (`QCC_ERR_TYPE`):** a non-constant operand — an identifier (an
enum constant needs the symbol table; deferred to semantic analysis), `sizeof` of
an *expression* (needs the operand's type), a call, subscript, member access,
assignment, or postfix `++`/`--` — a floating operand, the comma operator, and
division or remainder by zero.

**Model:** each subexpression evaluates to an internal `(bits, width, signedness)`
triple; operators promote each operand, apply the usual arithmetic conversions to a
common type, compute in 64-bit arithmetic, and reduce to the result width with C
wrap-around. Widths come from the same `type` module that backs `sizeof`
(ADR-0020), so there is one target model.

**Purity:** no allocation, no diagnostics, no symbol table. The caller decides
whether "not a constant" is an error: the array-bound parser treats it as an
unknown (incomplete) bound, while `enum`/`case`/`_Static_assert` (as they land)
report it with a source location.

**Dependencies:** `ast`, `type`, `token`, `status`.
