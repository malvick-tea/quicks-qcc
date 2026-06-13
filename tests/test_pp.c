/*
 * Tests for the preprocessor driver (ISO C11 §6.10): materialization of the
 * token stream, object-like macro definition and expansion, hide-set recursion
 * control (§6.10.3.4), #undef, the identical/non-identical redefinition rule
 * (§6.10.3 ¶2), and directive error handling. Phase-4 output carries no newline
 * tokens (line structure is recovered from provenance), so counts exclude them.
 */
#include "qtest.h"

#include <string.h>

#include "diag/diag.h"
#include "pp/pp.h"
#include "source/source.h"
#include "token/token.h"

/* Run the preprocessor over `text`; fills *out (caller calls cleanup). Returns
   the qcc_pp_run status. */
static qcc_status run(const char *text, qcc_pp *pp, qcc_diag_sink *diags,
                      qcc_source *src, qcc_ptok_list *out)
{
    qcc_diag_sink_init(diags);
    qcc_status st = qcc_source_from_memory("t.c", text, strlen(text), src);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "source init");
    QTEST_CHECK_EQ_INT(qcc_pp_init(pp, diags), QCC_OK, "pp init");
    qcc_ptok_list_init(out);
    return qcc_pp_run(pp, src, out);
}

static void cleanup(qcc_pp *pp, qcc_diag_sink *diags, qcc_source *src,
                    qcc_ptok_list *out)
{
    qcc_ptok_list_dispose(out);
    qcc_pp_dispose(pp);
    qcc_diag_sink_dispose(diags);
    qcc_source_dispose(src);
}

static size_t errors(const qcc_diag_sink *d)
{
    return qcc_diag_severity_count(d, QCC_DIAG_ERROR);
}
static size_t warnings(const qcc_diag_sink *d)
{
    return qcc_diag_severity_count(d, QCC_DIAG_WARNING);
}

/* Assert token i has the given kind and spelling. */
static void chk(const qcc_ptok_list *out, size_t i, qcc_pp_token_kind kind,
                const char *spelling)
{
    QTEST_CHECK_TRUE(i < out->count);
    if (i < out->count) {
        QTEST_CHECK_EQ_INT(out->items[i].kind, kind, "kind");
        QTEST_CHECK_SPAN(out->items[i].spelling, out->items[i].spelling_len,
                         spelling, "spelling");
    }
}

/* Materialization with no directives: newlines are dropped, EOF terminates. */
static void test_materialize_stream(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("int x = 42;\n", &pp, &d, &s, &out), QCC_OK, "run");

    /* int x = 42 ; EOF  (no NEWLINE token) */
    QTEST_CHECK_EQ_UINT(out.count, 6, "token count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "int");
    chk(&out, 1, QCC_PP_TOKEN_IDENTIFIER, "x");
    chk(&out, 2, QCC_PP_TOKEN_PUNCT, "=");
    chk(&out, 3, QCC_PP_TOKEN_PP_NUMBER, "42");
    chk(&out, 4, QCC_PP_TOKEN_PUNCT, ";");
    QTEST_CHECK_EQ_INT(out.items[5].kind, QCC_PP_TOKEN_EOF, "eof");
    QTEST_CHECK_TRUE(out.items[0].at_line_start);
    QTEST_CHECK_TRUE(out.items[2].leading_space);
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Object-like macro: the name is replaced by the replacement list. */
static void test_object_macro(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define N 42\nint x = N;\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* int x = 42 ; EOF */
    QTEST_CHECK_EQ_UINT(out.count, 6, "count");
    chk(&out, 3, QCC_PP_TOKEN_PP_NUMBER, "42");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* A multi-token replacement expands to all its tokens. */
static void test_object_multitoken(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define P a + b\nP\n", &pp, &d, &s, &out), QCC_OK, "run");
    /* a + b EOF */
    QTEST_CHECK_EQ_UINT(out.count, 4, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "a");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, "+");
    chk(&out, 2, QCC_PP_TOKEN_IDENTIFIER, "b");
    cleanup(&pp, &d, &s, &out);
}

/* Replacement is rescanned: a macro expanding to another macro fully expands. */
static void test_nested_expansion(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define A B\n#define B 1\nA\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* 1 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    cleanup(&pp, &d, &s, &out);
}

/* Self-referential macro does not loop (§6.10.3.4): x -> x, painted, stops. */
static void test_self_reference(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define x x\nx\n", &pp, &d, &s, &out), QCC_OK, "run");
    /* x EOF */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "x");
    cleanup(&pp, &d, &s, &out);
}

/* Mutually recursive object macros terminate (hide sets accumulate). */
static void test_mutual_recursion(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define a b\n#define b a\na\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* a -> b -> a (now painted) EOF */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "a");
    cleanup(&pp, &d, &s, &out);
}

/* #undef removes the definition; later uses are not expanded. */
static void test_undef(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define N 1\n#undef N\nN\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* N EOF (identifier, unexpanded) */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "N");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Identical redefinition is allowed silently; non-identical warns with a note. */
static void test_redefinition(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define N 1\n#define N 1\nN\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(warnings(&d), 0, "identical: no warning");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    cleanup(&pp, &d, &s, &out);

    QTEST_CHECK_EQ_INT(run("#define N 1\n#define N 2\nN\n", &pp, &d, &s, &out),
                       QCC_OK, "run2");
    QTEST_CHECK_EQ_UINT(warnings(&d), 1, "non-identical: one warning");
    /* The new definition takes effect. */
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "2");
    cleanup(&pp, &d, &s, &out);
}

/* A '#' that is not at the start of a file line is an ordinary punctuator. */
static void test_hash_not_directive(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("a # b\n", &pp, &d, &s, &out), QCC_OK, "run");
    /* a # b EOF */
    QTEST_CHECK_EQ_UINT(out.count, 4, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "a");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, "#");
    chk(&out, 2, QCC_PP_TOKEN_IDENTIFIER, "b");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Directive diagnostics: bad macro name, unknown directive; null directive is
   silent. Function-like definitions parse without error (expansion is a later
   step, so the name is left unexpanded here). */
static void test_directive_errors(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;

    QTEST_CHECK_EQ_INT(run("#define 123 x\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "non-identifier macro name");
    cleanup(&pp, &d, &s, &out);

    QTEST_CHECK_EQ_INT(run("#frobnicate 1\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "unknown directive");
    cleanup(&pp, &d, &s, &out);

    QTEST_CHECK_EQ_INT(run("#\nint a;\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "null directive is silent");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "int");
    cleanup(&pp, &d, &s, &out);

    QTEST_CHECK_EQ_INT(run("#define F(a, b) a b\nq\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "valid function-like define");
    cleanup(&pp, &d, &s, &out);
}

/* A '#' constraint inside a function-like replacement is diagnosed at define. */
static void test_stringize_constraint(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    /* '#' followed by a non-parameter is invalid (§6.10.3.2 ¶1). */
    QTEST_CHECK_EQ_INT(run("#define F(a) # b\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "# not before a parameter");
    cleanup(&pp, &d, &s, &out);

    /* '##' at the end of a replacement is invalid (§6.10.3.3 ¶1). */
    QTEST_CHECK_EQ_INT(run("#define G a ##\n", &pp, &d, &s, &out), QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "## at end");
    cleanup(&pp, &d, &s, &out);
}

/* Function-like macro: arguments substitute into the replacement. */
static void test_function_macro(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define ADD(a, b) a + b\nADD(1, 2)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* 1 + 2 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 4, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, "+");
    chk(&out, 2, QCC_PP_TOKEN_PP_NUMBER, "2");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* A function-like macro name not followed by '(' is left unexpanded (§6.10.3 ¶10). */
static void test_function_no_paren(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define F(x) x\nF + 1\n", &pp, &d, &s, &out), QCC_OK, "run");
    /* F + 1 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 4, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "F");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, "+");
    cleanup(&pp, &d, &s, &out);
}

/* Commas inside nested parens stay within one argument. */
static void test_nested_paren_arg(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define ID(x) x\nID((1, 2))\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* ( 1 , 2 ) EOF */
    QTEST_CHECK_EQ_UINT(out.count, 6, "count");
    chk(&out, 0, QCC_PP_TOKEN_PUNCT, "(");
    chk(&out, 2, QCC_PP_TOKEN_PUNCT, ",");
    chk(&out, 4, QCC_PP_TOKEN_PUNCT, ")");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Arguments are macro-expanded before substitution, except under # and ##. */
static void test_arg_prescan(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define A 1\n#define F(x) x\nF(A)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* A is expanded to 1 in the argument: 1 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    cleanup(&pp, &d, &s, &out);
}

/* # stringizes the UNexpanded argument (§6.10.3.2), escaping quotes. */
static void test_stringize(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define STR(x) #x\nSTR(a b)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    chk(&out, 0, QCC_PP_TOKEN_STRING_LIT, "\"a b\"");
    cleanup(&pp, &d, &s, &out);

    /* Unexpanded even when the argument is itself a macro. */
    QTEST_CHECK_EQ_INT(run("#define A 1\n#define STR(x) #x\nSTR(A)\n",
                           &pp, &d, &s, &out), QCC_OK, "run2");
    chk(&out, 0, QCC_PP_TOKEN_STRING_LIT, "\"A\"");
    cleanup(&pp, &d, &s, &out);

    /* Quotes and backslashes inside a string-literal argument are escaped. */
    QTEST_CHECK_EQ_INT(run("#define STR(x) #x\nSTR(\"hi\")\n", &pp, &d, &s, &out),
                       QCC_OK, "run3");
    chk(&out, 0, QCC_PP_TOKEN_STRING_LIT, "\"\\\"hi\\\"\"");
    cleanup(&pp, &d, &s, &out);
}

/* ## pastes the spellings of its operands into one token (§6.10.3.3). */
static void test_paste(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define CAT(a, b) a##b\nCAT(foo, bar)\n",
                           &pp, &d, &s, &out), QCC_OK, "run");
    /* foobar EOF */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "foobar");
    cleanup(&pp, &d, &s, &out);

    /* Pasting digits yields one pp-number; operands are unexpanded. */
    QTEST_CHECK_EQ_INT(run("#define A 9\n#define CAT(a, b) a##b\nCAT(1, 2)\n",
                           &pp, &d, &s, &out), QCC_OK, "run2");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "12");
    cleanup(&pp, &d, &s, &out);

    /* Pasting punctuators. */
    QTEST_CHECK_EQ_INT(run("#define G(a, b) a ## b\nG(+, +)\n", &pp, &d, &s, &out),
                       QCC_OK, "run3");
    chk(&out, 0, QCC_PP_TOKEN_PUNCT, "++");
    cleanup(&pp, &d, &s, &out);
}

/* An invalid paste is diagnosed (§6.10.3.3 ¶3). */
static void test_paste_invalid(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define BAD(a, b) a##b\nBAD(1, +)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "invalid paste diagnosed");
    cleanup(&pp, &d, &s, &out);
}

/* Variadic macros: __VA_ARGS__ captures the tail with its commas. */
static void test_variadic(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define PR(...) __VA_ARGS__\nPR(1, 2, 3)\n",
                           &pp, &d, &s, &out), QCC_OK, "run");
    /* 1 , 2 , 3 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 6, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, ",");
    chk(&out, 2, QCC_PP_TOKEN_PP_NUMBER, "2");
    cleanup(&pp, &d, &s, &out);

    /* A named parameter plus a (possibly empty) variadic tail. */
    QTEST_CHECK_EQ_INT(run("#define V(x, ...) x __VA_ARGS__\nV(1)\n",
                           &pp, &d, &s, &out), QCC_OK, "run2");
    /* 1 EOF (empty __VA_ARGS__) */
    QTEST_CHECK_EQ_UINT(out.count, 2, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    cleanup(&pp, &d, &s, &out);
}

/* Self-referential function-like macro terminates via the hide set. */
static void test_function_recursion(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define f(x) f(x)\nf(1)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* f ( 1 ) EOF — inner f is painted and not re-expanded */
    QTEST_CHECK_EQ_UINT(out.count, 5, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "f");
    chk(&out, 1, QCC_PP_TOKEN_PUNCT, "(");
    chk(&out, 2, QCC_PP_TOKEN_PP_NUMBER, "1");
    chk(&out, 3, QCC_PP_TOKEN_PUNCT, ")");
    cleanup(&pp, &d, &s, &out);
}

/* Wrong argument count is diagnosed. */
static void test_arg_count(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define F(a, b) a b\nF(1)\n", &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "too few arguments");
    cleanup(&pp, &d, &s, &out);
}

/* A function-like invocation may span lines (the '(' search crosses newlines). */
static void test_call_spanning_lines(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run("#define ADD(a, b) a + b\nADD\n(1,\n2)\n",
                           &pp, &d, &s, &out), QCC_OK, "run");
    /* 1 + 2 EOF */
    QTEST_CHECK_EQ_UINT(out.count, 4, "count");
    chk(&out, 0, QCC_PP_TOKEN_PP_NUMBER, "1");
    chk(&out, 2, QCC_PP_TOKEN_PP_NUMBER, "2");
    cleanup(&pp, &d, &s, &out);
}

/* Argument validation and NULL-safety. */
static void test_invalid_args(void)
{
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_pp pp;
    QTEST_CHECK_EQ_INT(qcc_pp_init(NULL, &d), QCC_ERR_INVALID_ARGUMENT, "null pp");
    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, NULL), QCC_ERR_INVALID_ARGUMENT, "null diags");
    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, &d), QCC_OK, "init");
    qcc_ptok_list out;
    qcc_ptok_list_init(&out);
    QTEST_CHECK_EQ_INT(qcc_pp_run(&pp, NULL, &out), QCC_ERR_INVALID_ARGUMENT, "null src");
    qcc_ptok_list_dispose(&out);
    qcc_pp_dispose(&pp);
    qcc_pp_dispose(NULL);
    qcc_diag_sink_dispose(&d);
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_materialize_stream();
    test_object_macro();
    test_object_multitoken();
    test_nested_expansion();
    test_self_reference();
    test_mutual_recursion();
    test_undef();
    test_redefinition();
    test_hash_not_directive();
    test_directive_errors();
    test_stringize_constraint();
    test_function_macro();
    test_function_no_paren();
    test_nested_paren_arg();
    test_arg_prescan();
    test_stringize();
    test_paste();
    test_paste_invalid();
    test_variadic();
    test_function_recursion();
    test_arg_count();
    test_call_spanning_lines();
    test_invalid_args();
    return qtest_report("pp");
}
