/*
 * Tests for the convert stage (ISO C11 §5.1.1.2 phases 5-7, Unit A): turning
 * preprocessing tokens into tokens — keyword resolution (§6.4.1), pp-number
 * classification into integer/floating constants (§6.4.4), the trivial category
 * shifts, and stray-token diagnostics (§6.4 ¶3). Keywords are resolved AFTER
 * preprocessing, so a macro may produce one. Constant values are a later unit;
 * here a constant token carries its lexeme.
 */
#include "qtest.h"

#include <stdint.h>
#include <string.h>

#include "convert/convert.h"
#include "diag/diag.h"
#include "pp/pp.h"
#include "source/source.h"
#include "token/token.h"

/* Holds the converted token stream and its interner alive for inspection. */
typedef struct cvctx {
    qcc_diag_sink  d;
    qcc_convert    cv;
    qcc_token_list toks;
    size_t         errors;
} cvctx;

/* Preprocess `text` then convert it; fills *c (caller calls ctx_free). The
   preprocessor and source are torn down here — convert re-interns spellings, so
   the token stream stays valid (its `source` pointers are unused by these tests). */
static void do_convert(const char *text, cvctx *c)
{
    qcc_diag_sink_init(&c->d);

    qcc_source s;
    QTEST_CHECK_EQ_INT(qcc_source_from_memory("t.c", text, strlen(text), &s),
                       QCC_OK, "source");
    qcc_pp pp;
    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, &c->d), QCC_OK, "pp init");
    qcc_ptok_list pt;
    qcc_ptok_list_init(&pt);
    QTEST_CHECK_EQ_INT(qcc_pp_run(&pp, &s, &pt), QCC_OK, "pp run");

    QTEST_CHECK_EQ_INT(qcc_convert_init(&c->cv, &c->d), QCC_OK, "convert init");
    qcc_token_list_init(&c->toks);
    QTEST_CHECK_EQ_INT(qcc_convert_run(&c->cv, &pt, &c->toks), QCC_OK, "convert run");
    c->errors = qcc_diag_severity_count(&c->d, QCC_DIAG_ERROR);

    qcc_ptok_list_dispose(&pt);
    qcc_pp_dispose(&pp);
    qcc_source_dispose(&s);
}

static void ctx_free(cvctx *c)
{
    qcc_token_list_dispose(&c->toks);
    qcc_convert_dispose(&c->cv);
    qcc_diag_sink_dispose(&c->d);
}

static void chk(const qcc_token_list *t, size_t i, qcc_token_kind kind,
                const char *spelling)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        QTEST_CHECK_EQ_INT(t->items[i].kind, kind, "kind");
        QTEST_CHECK_SPAN(t->items[i].spelling, t->items[i].spelling_len,
                         spelling, "spelling");
    }
}

/* A declaration converts to keyword/identifier/punct/integer/eof. */
static void test_basic_stream(void)
{
    cvctx c;
    do_convert("int x = 42;\n", &c);
    /* int x = 42 ; EOF */
    QTEST_CHECK_EQ_UINT(c.toks.count, 6, "count");
    chk(&c.toks, 0, QCC_TOKEN_KEYWORD, "int");
    QTEST_CHECK_EQ_INT(c.toks.items[0].keyword, QCC_KW_INT, "int keyword");
    chk(&c.toks, 1, QCC_TOKEN_IDENTIFIER, "x");
    chk(&c.toks, 2, QCC_TOKEN_PUNCT, "=");
    QTEST_CHECK_EQ_INT(c.toks.items[2].punct, QCC_PUNCT_EQ, "= punct");
    chk(&c.toks, 3, QCC_TOKEN_INTEGER, "42");
    chk(&c.toks, 4, QCC_TOKEN_PUNCT, ";");
    QTEST_CHECK_EQ_INT(c.toks.items[5].kind, QCC_TOKEN_EOF, "eof");
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

/* Keyword recognition across the table, including the _-keywords. */
static void test_keywords(void)
{
    cvctx c;
    do_convert("while xyz _Bool _Static_assert return\n", &c);
    chk(&c.toks, 0, QCC_TOKEN_KEYWORD, "while");
    QTEST_CHECK_EQ_INT(c.toks.items[0].keyword, QCC_KW_WHILE, "while");
    chk(&c.toks, 1, QCC_TOKEN_IDENTIFIER, "xyz");
    chk(&c.toks, 2, QCC_TOKEN_KEYWORD, "_Bool");
    QTEST_CHECK_EQ_INT(c.toks.items[2].keyword, QCC_KW_BOOL, "_Bool");
    chk(&c.toks, 3, QCC_TOKEN_KEYWORD, "_Static_assert");
    QTEST_CHECK_EQ_INT(c.toks.items[3].keyword, QCC_KW_STATIC_ASSERT, "_Static_assert");
    chk(&c.toks, 4, QCC_TOKEN_KEYWORD, "return");
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

/* A keyword produced by macro expansion is still resolved (convert is phase 7). */
static void test_keyword_from_macro(void)
{
    cvctx c;
    do_convert("#define T int\nT y;\n", &c);
    chk(&c.toks, 0, QCC_TOKEN_KEYWORD, "int");
    QTEST_CHECK_EQ_INT(c.toks.items[0].keyword, QCC_KW_INT, "macro->keyword");
    chk(&c.toks, 1, QCC_TOKEN_IDENTIFIER, "y");
    ctx_free(&c);
}

/* pp-numbers classify into integer vs floating by shape (§6.4.8 -> §6.4.4). */
static void test_number_classification(void)
{
    cvctx c;
    do_convert("42 0x1f 010 3.14 1e10 0x1p4 .5 0xE5\n", &c);
    chk(&c.toks, 0, QCC_TOKEN_INTEGER, "42");
    chk(&c.toks, 1, QCC_TOKEN_INTEGER, "0x1f");
    chk(&c.toks, 2, QCC_TOKEN_INTEGER, "010");
    chk(&c.toks, 3, QCC_TOKEN_FLOATING, "3.14");
    chk(&c.toks, 4, QCC_TOKEN_FLOATING, "1e10");
    chk(&c.toks, 5, QCC_TOKEN_FLOATING, "0x1p4");
    chk(&c.toks, 6, QCC_TOKEN_FLOATING, ".5");
    chk(&c.toks, 7, QCC_TOKEN_INTEGER, "0xE5"); /* 'e' is a hex digit, not exp */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

/* Character constants and string literals keep their lexemes. */
static void test_char_and_string(void)
{
    cvctx c;
    do_convert("'a' \"hi\" L'x' u8\"s\"\n", &c);
    chk(&c.toks, 0, QCC_TOKEN_CHAR, "'a'");
    chk(&c.toks, 1, QCC_TOKEN_STRING, "\"hi\"");
    chk(&c.toks, 2, QCC_TOKEN_CHAR, "L'x'");
    chk(&c.toks, 3, QCC_TOKEN_STRING, "u8\"s\"");
    ctx_free(&c);
}

/* Multi-character punctuators carry the right enumerator. */
static void test_punctuators(void)
{
    cvctx c;
    do_convert("-> ++ << ... %:\n", &c);
    chk(&c.toks, 0, QCC_TOKEN_PUNCT, "->");
    QTEST_CHECK_EQ_INT(c.toks.items[0].punct, QCC_PUNCT_ARROW, "arrow");
    QTEST_CHECK_EQ_INT(c.toks.items[1].punct, QCC_PUNCT_PLUS_PLUS, "++");
    QTEST_CHECK_EQ_INT(c.toks.items[2].punct, QCC_PUNCT_LSHIFT, "<<");
    QTEST_CHECK_EQ_INT(c.toks.items[3].punct, QCC_PUNCT_ELLIPSIS, "...");
    /* %: is a digraph for # — same enumerator, original spelling kept. */
    QTEST_CHECK_EQ_INT(c.toks.items[4].punct, QCC_PUNCT_HASH, "%: -> #");
    ctx_free(&c);
}

/* A stray token §6.4 ¶3 forbids is diagnosed and dropped. */
static void test_stray_token(void)
{
    cvctx c;
    do_convert("int @ x\n", &c);
    QTEST_CHECK_EQ_UINT(c.errors, 1, "one stray-token error");
    /* int x EOF — the '@' is dropped. */
    chk(&c.toks, 0, QCC_TOKEN_KEYWORD, "int");
    chk(&c.toks, 1, QCC_TOKEN_IDENTIFIER, "x");
    QTEST_CHECK_EQ_INT(c.toks.items[2].kind, QCC_TOKEN_EOF, "eof");
    ctx_free(&c);
}

/* Integer constants get a value and a type (§6.4.4.1). */
static void chk_int(const qcc_token_list *t, size_t i, uint64_t value,
                    qcc_int_type type)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        QTEST_CHECK_EQ_INT(t->items[i].kind, QCC_TOKEN_INTEGER, "integer kind");
        QTEST_CHECK_EQ_UINT(t->items[i].int_value, value, "int value");
        QTEST_CHECK_EQ_INT(t->items[i].int_type, type, "int type");
    }
}

static void test_integer_values(void)
{
    cvctx c;
    /* Bases. */
    do_convert("0 42 0xFF 010 0x0\n", &c);
    chk_int(&c.toks, 0, 0u, QCC_INT_INT);
    chk_int(&c.toks, 1, 42u, QCC_INT_INT);
    chk_int(&c.toks, 2, 255u, QCC_INT_INT);
    chk_int(&c.toks, 3, 8u, QCC_INT_INT);   /* octal 010 */
    chk_int(&c.toks, 4, 0u, QCC_INT_INT);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* Type widening by value (LP64): decimal picks signed types. */
    do_convert("2147483647 2147483648 4294967296\n", &c);
    chk_int(&c.toks, 0, 2147483647u, QCC_INT_INT);          /* INT_MAX     */
    chk_int(&c.toks, 1, 2147483648u, QCC_INT_LONG);         /* > INT_MAX   */
    chk_int(&c.toks, 2, 4294967296u, QCC_INT_LONG);
    ctx_free(&c);

    /* Hexadecimal without a suffix may pick unsigned int. */
    do_convert("0xFFFFFFFF 0x100000000\n", &c);
    chk_int(&c.toks, 0, 4294967295u, QCC_INT_UINT);         /* UINT_MAX    */
    chk_int(&c.toks, 1, 4294967296u, QCC_INT_LONG);
    ctx_free(&c);

    /* Suffixes. */
    do_convert("42u 42L 42UL 42ll 42ull 0x10ULL\n", &c);
    chk_int(&c.toks, 0, 42u, QCC_INT_UINT);
    chk_int(&c.toks, 1, 42u, QCC_INT_LONG);
    chk_int(&c.toks, 2, 42u, QCC_INT_ULONG);
    chk_int(&c.toks, 3, 42u, QCC_INT_LLONG);
    chk_int(&c.toks, 4, 42u, QCC_INT_ULLONG);
    chk_int(&c.toks, 5, 16u, QCC_INT_ULLONG);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* The widest value, and a decimal value too large for any signed type. */
    do_convert("18446744073709551615 9223372036854775808\n", &c);
    chk_int(&c.toks, 0, 18446744073709551615u, QCC_INT_ULLONG); /* ULLONG_MAX */
    /* 2^63 has no signed type; becomes unsigned long long with a warning. */
    chk_int(&c.toks, 1, 9223372036854775808u, QCC_INT_ULLONG);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "large constants are warnings, not errors");
    ctx_free(&c);
}

/* Malformed integer constants are diagnosed. */
static void test_integer_errors(void)
{
    cvctx c;
    do_convert("08\n", &c); /* 8 is not an octal digit. */
    QTEST_CHECK_EQ_UINT(c.errors, 1, "invalid octal digit");
    ctx_free(&c);

    do_convert("42z\n", &c); /* z is not a valid suffix. */
    QTEST_CHECK_EQ_UINT(c.errors, 1, "invalid suffix");
    ctx_free(&c);

    do_convert("0x\n", &c); /* no hex digits. */
    QTEST_CHECK_EQ_UINT(c.errors, 1, "empty hex constant");
    ctx_free(&c);
}

/* Floating constants get a value and a type (§6.4.4.2). */
static void chk_float(const qcc_token_list *t, size_t i, double value,
                      qcc_float_type type)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        QTEST_CHECK_EQ_INT(t->items[i].kind, QCC_TOKEN_FLOATING, "floating kind");
        QTEST_CHECK_TRUE(t->items[i].float_value == value);
        QTEST_CHECK_EQ_INT(t->items[i].float_type, type, "float type");
    }
}

static void test_floating_values(void)
{
    cvctx c;
    /* Decimal forms and the default double type (values are exact in binary). */
    do_convert("1.5 .5 1e2 1.0 0x1p4\n", &c);
    chk_float(&c.toks, 0, 1.5, QCC_FLOAT_DOUBLE);
    chk_float(&c.toks, 1, 0.5, QCC_FLOAT_DOUBLE);
    chk_float(&c.toks, 2, 100.0, QCC_FLOAT_DOUBLE);
    chk_float(&c.toks, 3, 1.0, QCC_FLOAT_DOUBLE);
    chk_float(&c.toks, 4, 16.0, QCC_FLOAT_DOUBLE);  /* hex float */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* Suffixes fix the type. */
    do_convert("1.0f 2.5F 3.0l 4.0L\n", &c);
    chk_float(&c.toks, 0, 1.0, QCC_FLOAT_FLOAT);
    chk_float(&c.toks, 1, 2.5, QCC_FLOAT_FLOAT);
    chk_float(&c.toks, 2, 3.0, QCC_FLOAT_LDOUBLE);
    chk_float(&c.toks, 3, 4.0, QCC_FLOAT_LDOUBLE);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

/* Character constants get a value and an encoding (§6.4.4.4). */
static void chk_char(const qcc_token_list *t, size_t i, int64_t value,
                     qcc_char_encoding enc)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        QTEST_CHECK_EQ_INT(t->items[i].kind, QCC_TOKEN_CHAR, "char kind");
        QTEST_CHECK_EQ_INT((int64_t)t->items[i].int_value, value, "char value");
        QTEST_CHECK_EQ_INT(t->items[i].char_encoding, enc, "char encoding");
    }
}

static void test_char_values(void)
{
    cvctx c;
    /* Plain constants: int value of the (signed) char; escapes; octal/hex. */
    do_convert("'a' '\\n' '\\0' '\\101' '\\x41'\n", &c);
    chk_char(&c.toks, 0, 'a', QCC_ENC_PLAIN);   /* 97               */
    chk_char(&c.toks, 1, 10, QCC_ENC_PLAIN);    /* newline          */
    chk_char(&c.toks, 2, 0, QCC_ENC_PLAIN);     /* NUL              */
    chk_char(&c.toks, 3, 65, QCC_ENC_PLAIN);    /* octal  \101 = A  */
    chk_char(&c.toks, 4, 65, QCC_ENC_PLAIN);    /* hex    \x41 = A  */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* char is signed on the target: a high-bit byte sign-extends to int. */
    do_convert("'\\xFF'\n", &c);
    chk_char(&c.toks, 0, -1, QCC_ENC_PLAIN);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* Multi-character constant: implementation-defined, packed big-endian; a
       warning, not an error. */
    do_convert("'ab'\n", &c);
    chk_char(&c.toks, 0, 0x6162, QCC_ENC_PLAIN); /* 'a'<<8 | 'b'    */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "multi-char is a warning");
    ctx_free(&c);

    /* Wide/u/U constants carry the prefix encoding; a UCN is one code point. */
    do_convert("L'x' u'A' U'A' L'\\u00e9' U'\\U0001F600'\n", &c);
    chk_char(&c.toks, 0, 'x', QCC_ENC_WIDE);
    chk_char(&c.toks, 1, 'A', QCC_ENC_CHAR16);
    chk_char(&c.toks, 2, 'A', QCC_ENC_CHAR32);
    chk_char(&c.toks, 3, 0xE9, QCC_ENC_WIDE);      /* é, single unit  */
    chk_char(&c.toks, 4, 0x1F600, QCC_ENC_CHAR32); /* 😀, fits 32-bit  */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* A wide multi-character constant keeps the last unit (impl-defined, warn). */
    do_convert("L'ab'\n", &c);
    chk_char(&c.toks, 0, 'b', QCC_ENC_WIDE);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "wide multi-char is a warning");
    ctx_free(&c);
}

/* Malformed character constants are diagnosed. */
static void test_char_errors(void)
{
    cvctx c;
    do_convert("'\\x100'\n", &c); /* 256 does not fit unsigned char. */
    QTEST_CHECK_EQ_UINT(c.errors, 1, "narrow escape out of range");
    ctx_free(&c);

    do_convert("u'\\U0001F600'\n", &c); /* non-BMP in a 16-bit char16_t. */
    QTEST_CHECK_EQ_UINT(c.errors, 1, "char16 out of range");
    ctx_free(&c);
}

/* String literals carry decoded code units and a terminator (§6.4.5). A narrow
   string is UTF-8 bytes; str_len excludes the appended NUL. */
static void chk_str8(const qcc_token_list *t, size_t i, qcc_char_encoding enc,
                     const char *bytes, size_t n)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        const qcc_token     *tk = &t->items[i];
        const unsigned char *d  = (const unsigned char *)tk->str_data;
        QTEST_CHECK_EQ_INT(tk->kind, QCC_TOKEN_STRING, "string kind");
        QTEST_CHECK_EQ_INT(tk->char_encoding, enc, "string encoding");
        QTEST_CHECK_EQ_UINT(tk->str_len, n, "string length");
        int ok = (d != NULL);
        for (size_t k = 0; ok && k < n; ++k) {
            ok = (d[k] == (unsigned char)bytes[k]);
        }
        QTEST_CHECK_TRUE(ok);
        if (d != NULL) {
            QTEST_CHECK_EQ_INT(d[n], 0, "NUL terminator"); /* §6.4.5 ¶6 */
        }
    }
}

/* Compare a wide (32-bit unit) string against expected code units. */
static void chk_str32(const qcc_token_list *t, size_t i, qcc_char_encoding enc,
                      const uint32_t *units, size_t n)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        const qcc_token *tk = &t->items[i];
        const uint32_t  *d  = (const uint32_t *)tk->str_data;
        QTEST_CHECK_EQ_INT(tk->char_encoding, enc, "wide string encoding");
        QTEST_CHECK_EQ_UINT(tk->str_len, n, "wide string length");
        int ok = (d != NULL);
        for (size_t k = 0; ok && k < n; ++k) {
            ok = (d[k] == units[k]);
        }
        QTEST_CHECK_TRUE(ok);
        if (d != NULL) {
            QTEST_CHECK_EQ_UINT(d[n], 0u, "wide NUL terminator");
        }
    }
}

/* Compare a char16_t (16-bit unit) string against expected code units. */
static void chk_str16(const qcc_token_list *t, size_t i, const uint16_t *units,
                      size_t n)
{
    QTEST_CHECK_TRUE(i < t->count);
    if (i < t->count) {
        const qcc_token *tk = &t->items[i];
        const uint16_t  *d  = (const uint16_t *)tk->str_data;
        QTEST_CHECK_EQ_INT(tk->char_encoding, QCC_ENC_CHAR16, "u string encoding");
        QTEST_CHECK_EQ_UINT(tk->str_len, n, "u string length");
        int ok = (d != NULL);
        for (size_t k = 0; ok && k < n; ++k) {
            ok = (d[k] == units[k]);
        }
        QTEST_CHECK_TRUE(ok);
        if (d != NULL) {
            QTEST_CHECK_EQ_UINT(d[n], 0u, "u NUL terminator");
        }
    }
}

static void test_string_values(void)
{
    cvctx c;
    /* Plain strings, escapes, and the empty string (just a terminator). The `;`
       separators keep these from concatenating (adjacency is tested elsewhere);
       so the string tokens land at even indices. */
    do_convert("\"hi\"; \"\\101\\102\"; \"a\\tb\"; \"\";\n", &c);
    chk_str8(&c.toks, 0, QCC_ENC_PLAIN, "hi", 2);
    chk_str8(&c.toks, 2, QCC_ENC_PLAIN, "AB", 2);        /* octal escapes    */
    chk_str8(&c.toks, 4, QCC_ENC_PLAIN, "a\tb", 3);
    chk_str8(&c.toks, 6, QCC_ENC_PLAIN, "", 0);          /* "" -> just NUL   */
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* A universal character name is UTF-8-encoded in a narrow string. */
    do_convert("\"\\u00e9\"\n", &c); /* é = U+00E9 -> C3 A9 */
    chk_str8(&c.toks, 0, QCC_ENC_PLAIN, "\xC3\xA9", 2);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* A hex escape is a raw byte, not a code point: \xff stays one byte. */
    do_convert("\"\\xff\"\n", &c);
    chk_str8(&c.toks, 0, QCC_ENC_PLAIN, "\xFF", 1);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

static void test_string_encodings(void)
{
    cvctx c;
    /* Wide and UTF-32 strings: one 32-bit unit per character. Separated by `;`
       so the differing prefixes do not trigger a concatenation mismatch. */
    do_convert("L\"AB\"; U\"A\";\n", &c);
    {
        const uint32_t ab[] = { 'A', 'B' };
        const uint32_t a[]  = { 'A' };
        chk_str32(&c.toks, 0, QCC_ENC_WIDE, ab, 2);
        chk_str32(&c.toks, 2, QCC_ENC_CHAR32, a, 1);
    }
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* char16_t with a non-BMP code point becomes a UTF-16 surrogate pair. */
    do_convert("u\"\\U0001F600\"\n", &c);
    {
        const uint16_t pair[] = { 0xD83D, 0xDE00 };
        chk_str16(&c.toks, 0, pair, 2);
    }
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* u8 string: a UCN is UTF-8-encoded into bytes. */
    do_convert("u8\"\\u00e9\"\n", &c);
    chk_str8(&c.toks, 0, QCC_ENC_UTF8, "\xC3\xA9", 2);
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);
}

/* Adjacent string literals are concatenated (phase 6, §6.4.5 ¶5). */
static void test_string_concat(void)
{
    cvctx c;
    /* Three plain pieces become one literal; the result has a single EOF after. */
    do_convert("\"ab\" \"cd\" \"e\"\n", &c);
    QTEST_CHECK_EQ_UINT(c.toks.count, 2, "one string + EOF");
    chk_str8(&c.toks, 0, QCC_ENC_PLAIN, "abcde", 5);
    QTEST_CHECK_EQ_INT(c.toks.items[1].kind, QCC_TOKEN_EOF, "eof");
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* Concatenated escapes decode across the join: "\101" "B" is "AB". */
    do_convert("\"\\101\" \"B\"\n", &c);
    chk_str8(&c.toks, 0, QCC_ENC_PLAIN, "AB", 2);
    ctx_free(&c);

    /* An unprefixed piece takes the wide prefix of its neighbour. */
    do_convert("\"a\" L\"b\"\n", &c);
    {
        const uint32_t ab[] = { 'a', 'b' };
        chk_str32(&c.toks, 0, QCC_ENC_WIDE, ab, 2);
    }
    QTEST_CHECK_EQ_UINT(c.errors, 0, "no errors");
    ctx_free(&c);

    /* Two different non-empty prefixes are a diagnosed mix. */
    do_convert("L\"a\" u\"b\"\n", &c);
    QTEST_CHECK_EQ_UINT(c.errors, 1, "mismatched encoding prefixes");
    ctx_free(&c);
}

/* An empty translation unit converts to just EOF. */
static void test_empty(void)
{
    cvctx c;
    do_convert("\n", &c);
    QTEST_CHECK_EQ_UINT(c.toks.count, 1, "just EOF");
    QTEST_CHECK_EQ_INT(c.toks.items[0].kind, QCC_TOKEN_EOF, "eof");
    ctx_free(&c);
}

/* Argument validation. */
static void test_invalid_args(void)
{
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_convert cv;
    QTEST_CHECK_EQ_INT(qcc_convert_init(NULL, &d), QCC_ERR_INVALID_ARGUMENT, "null cv");
    QTEST_CHECK_EQ_INT(qcc_convert_init(&cv, NULL), QCC_ERR_INVALID_ARGUMENT, "null diags");
    QTEST_CHECK_EQ_INT(qcc_convert_init(&cv, &d), QCC_OK, "init");
    qcc_token_list out;
    qcc_token_list_init(&out);
    QTEST_CHECK_EQ_INT(qcc_convert_run(&cv, NULL, &out), QCC_ERR_INVALID_ARGUMENT, "null in");
    qcc_token_list_dispose(&out);
    qcc_convert_dispose(&cv);
    qcc_convert_dispose(NULL);
    qcc_diag_sink_dispose(&d);
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_basic_stream();
    test_keywords();
    test_keyword_from_macro();
    test_number_classification();
    test_char_and_string();
    test_punctuators();
    test_stray_token();
    test_integer_values();
    test_integer_errors();
    test_floating_values();
    test_char_values();
    test_char_errors();
    test_string_values();
    test_string_encodings();
    test_string_concat();
    test_empty();
    test_invalid_args();
    return qtest_report("convert");
}
