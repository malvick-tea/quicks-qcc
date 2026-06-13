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
    test_invalid_args();
    return qtest_report("pp");
}
