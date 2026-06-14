/*
 * Tests for the `-E` renderer (pp/render): turning a phase-4 qcc_ptok stream
 * back into text. The contract is that the output re-lexes to the same tokens —
 * lines reconstructed from at_line_start, original inter-token spacing kept, and
 * a space forced wherever two tokens would otherwise paste (§6.4 maximal munch).
 */
#include "qtest.h"

#include <stdlib.h>
#include <string.h>

#include "diag/diag.h"
#include "pp/pp.h"
#include "pp/render.h"
#include "source/source.h"

/* Preprocess `src` (as "t.c") and render the result. Returns a malloc'd string
   (caller frees) and, via *out_errors, the error count. */
static char *render_of(const char *src, size_t *out_errors)
{
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_source s;
    QTEST_CHECK_EQ_INT(qcc_source_from_memory("t.c", src, strlen(src), &s),
                       QCC_OK, "source");
    qcc_pp pp;
    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, &d), QCC_OK, "pp init");

    qcc_ptok_list toks;
    qcc_ptok_list_init(&toks);
    QTEST_CHECK_EQ_INT(qcc_pp_run(&pp, &s, &toks), QCC_OK, "run");

    char  *text = NULL;
    size_t len  = 0;
    QTEST_CHECK_EQ_INT(qcc_pp_render(&toks, &text, &len), QCC_OK, "render");
    if (text != NULL) {
        QTEST_CHECK_EQ_UINT(strlen(text), len, "len matches strlen");
    }
    if (out_errors != NULL) {
        *out_errors = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);
    }

    qcc_ptok_list_dispose(&toks);
    qcc_pp_dispose(&pp);
    qcc_diag_sink_dispose(&d);
    qcc_source_dispose(&s);
    return text;
}

static void expect_render(const char *src, const char *expected)
{
    char *got = render_of(src, NULL);
    QTEST_CHECK_TRUE(got != NULL);
    if (got != NULL) {
        int eq = (strcmp(got, expected) == 0);
        QTEST_CHECK_TRUE(eq);
        if (!eq) {
            fprintf(stderr, "  render got=[%s] want=[%s]\n", got, expected);
        }
        free(got);
    }
}

/* Plain text round-trips, with original spacing and a trailing newline. */
static void test_plain(void)
{
    expect_render("int x = 42;\n", "int x = 42;\n");
    expect_render("char c = 'a';\n", "char c = 'a';\n");
}

/* Each source line becomes one output line. */
static void test_lines(void)
{
    expect_render("int a;\nint b;\n", "int a;\nint b;\n");
}

/* An empty / directive-only translation unit renders to nothing. */
static void test_empty(void)
{
    expect_render("\n", "");
    expect_render("#define N 1\n", "");
}

/* Object-like macro expansion is inlined on the invocation line. */
static void test_macro_inline(void)
{
    expect_render("#define N 42\nint x = N;\n", "int x = 42;\n");
    expect_render("#define P a b\nP\n", "a b\n");
}

/* A macro that begins a line still renders at the start of its line. */
static void test_macro_line_start(void)
{
    expect_render("x\n#define P +\nP\n", "x\n+\n");
}

/* Adjacent tokens that would paste get a separating space (re-lexability). */
static void test_no_accidental_paste(void)
{
    /* E + + : the two '+' must not become '++'. */
    expect_render("#define E +\nE+\n", "+ +\n");
}

/* Conditional inclusion: only the taken group is rendered. */
static void test_conditional(void)
{
    expect_render("#if 1\nyes\n#else\nno\n#endif\n", "yes\n");
    expect_render("#if 0\nyes\n#else\nno\n#endif\n", "no\n");
}

/* NULL-argument contract. */
static void test_invalid_args(void)
{
    char  *text = NULL;
    size_t len  = 0;
    QTEST_CHECK_EQ_INT(qcc_pp_render(NULL, &text, &len), QCC_ERR_INVALID_ARGUMENT,
                       "null list");
    qcc_ptok_list toks;
    qcc_ptok_list_init(&toks);
    QTEST_CHECK_EQ_INT(qcc_pp_render(&toks, NULL, &len), QCC_ERR_INVALID_ARGUMENT,
                       "null out");
    qcc_ptok_list_dispose(&toks);
}

int main(void)
{
    test_plain();
    test_lines();
    test_empty();
    test_macro_inline();
    test_macro_line_start();
    test_no_accidental_paste();
    test_conditional();
    test_invalid_args();
    return qtest_report("render");
}
