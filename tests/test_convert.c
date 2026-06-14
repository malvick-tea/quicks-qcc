/*
 * Tests for the convert stage (ISO C11 §5.1.1.2 phases 5-7, Unit A): turning
 * preprocessing tokens into tokens — keyword resolution (§6.4.1), pp-number
 * classification into integer/floating constants (§6.4.4), the trivial category
 * shifts, and stray-token diagnostics (§6.4 ¶3). Keywords are resolved AFTER
 * preprocessing, so a macro may produce one. Constant values are a later unit;
 * here a constant token carries its lexeme.
 */
#include "qtest.h"

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
    test_empty();
    test_invalid_args();
    return qtest_report("convert");
}
