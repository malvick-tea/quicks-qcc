/*
 * Tests for #include (ISO C11 §6.10.2): the two literal forms (<...> and
 * "..."), the search order (angle dirs for <...>; includer dir then quote dirs
 * then the angle fallback for "..."), nested and subdirectory includes, the
 * computed-include form (§6.10.2 ¶4), provenance of __FILE__/__LINE__ inside an
 * included file (§6.10.8.1), and the error/limit paths (not found, missing
 * filename, the recursion cap of ADR-0015).
 *
 * The header fixtures are written into the build tree by tests/CMakeLists.txt;
 * their directories arrive here as QCC_INC_ANGLE_DIR / QCC_INC_QUOTE_DIR so the
 * test never depends on the current working directory. Phase-4 output carries no
 * newline tokens, so token counts exclude them.
 */
#include "qtest.h"

#include <string.h>

#include "diag/diag.h"
#include "pp/pp.h"
#include "source/source.h"
#include "token/token.h"

#ifndef QCC_INC_ANGLE_DIR
#define QCC_INC_ANGLE_DIR ""
#endif
#ifndef QCC_INC_QUOTE_DIR
#define QCC_INC_QUOTE_DIR ""
#endif

/*
 * Preprocess `text` as the translation unit "t.c", with `angle_dir` and
 * `quote_dir` (each may be NULL) added to the search path. Fills *out (the
 * caller calls cleanup). Returns the qcc_pp_run status.
 */
static qcc_status run_inc(const char *text, const char *angle_dir,
                          const char *quote_dir, qcc_pp *pp, qcc_diag_sink *diags,
                          qcc_source *src, qcc_ptok_list *out)
{
    qcc_diag_sink_init(diags);
    qcc_status st = qcc_source_from_memory("t.c", text, strlen(text), src);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "source init");
    QTEST_CHECK_EQ_INT(qcc_pp_init(pp, diags), QCC_OK, "pp init");
    if (angle_dir != NULL) {
        QTEST_CHECK_EQ_INT(qcc_pp_add_include_dir(pp, angle_dir), QCC_OK,
                           "add angle dir");
    }
    if (quote_dir != NULL) {
        QTEST_CHECK_EQ_INT(qcc_pp_add_quote_include_dir(pp, quote_dir), QCC_OK,
                           "add quote dir");
    }
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

/* Does any token's (NUL-terminated, interned) spelling contain `needle`? */
static int any_spelling_contains(const qcc_ptok_list *out, const char *needle)
{
    for (size_t i = 0; i < out->count; ++i) {
        if (strstr(out->items[i].spelling, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* <...> is found in an angle dir, and its tokens precede the includer's. */
static void test_angle_found(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <only_angle.h>\nint x;\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* int from_angle ; int x ; EOF */
    QTEST_CHECK_EQ_UINT(out.count, 7, "count");
    chk(&out, 0, QCC_PP_TOKEN_IDENTIFIER, "int");
    chk(&out, 1, QCC_PP_TOKEN_IDENTIFIER, "from_angle");
    chk(&out, 2, QCC_PP_TOKEN_PUNCT, ";");
    chk(&out, 3, QCC_PP_TOKEN_IDENTIFIER, "int");
    chk(&out, 4, QCC_PP_TOKEN_IDENTIFIER, "x");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* "..." is found in a quote dir. */
static void test_quote_found(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include \"only_quote.h\"\n",
                               NULL, QCC_INC_QUOTE_DIR, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_TRUE(any_spelling_contains(&out, "from_quote"));
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* "..." falls back to the angle dirs when not found earlier (§6.10.2 ¶3). */
static void test_quote_falls_back_to_angle(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    /* common.h exists only in the angle dir. */
    QTEST_CHECK_EQ_INT(run_inc("#include \"common.h\"\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_TRUE(any_spelling_contains(&out, "common_angle"));
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* <...> does NOT search the quote dirs: a quote-only header is not found. */
static void test_angle_ignores_quote_dir(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <only_quote.h>\n",
                               NULL, QCC_INC_QUOTE_DIR, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "angle form: quote-only header not found");
    cleanup(&pp, &d, &s, &out);
}

/* A nested include resumes the includer after the included file ends. */
static void test_nested_include(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <chain1.h>\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* chain1.h includes chain2.h first: int chain2 ; int chain1 ; EOF */
    QTEST_CHECK_EQ_UINT(out.count, 7, "count");
    chk(&out, 1, QCC_PP_TOKEN_IDENTIFIER, "chain2");
    chk(&out, 4, QCC_PP_TOKEN_IDENTIFIER, "chain1");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* A header name may carry a subdirectory component. */
static void test_subdir_include(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <sub/deep.h>\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_TRUE(any_spelling_contains(&out, "deep"));
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* __FILE__ / __LINE__ inside an included file report THAT file (§6.10.8.1). */
static void test_file_line_in_include(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <filewithloc.h>\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    /* Line 1 is __FILE__ (the included path), line 2 is __LINE__ (the number 2). */
    QTEST_CHECK_TRUE(out.count >= 2);
    QTEST_CHECK_EQ_INT(out.items[0].kind, QCC_PP_TOKEN_STRING_LIT, "file kind");
    QTEST_CHECK_TRUE(strstr(out.items[0].spelling, "filewithloc.h") != NULL);
    chk(&out, 1, QCC_PP_TOKEN_PP_NUMBER, "2");
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Computed include: a macro that expands to a string literal (§6.10.2 ¶4). */
static void test_computed_include_string(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#define HDR \"only_quote.h\"\n#include HDR\n",
                               NULL, QCC_INC_QUOTE_DIR, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_TRUE(any_spelling_contains(&out, "from_quote"));
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* Computed include: a macro that expands to a < ... > bracketing. */
static void test_computed_include_angle(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#define HDR <only_angle.h>\n#include HDR\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_TRUE(any_spelling_contains(&out, "from_angle"));
    QTEST_CHECK_EQ_UINT(errors(&d), 0, "no errors");
    cleanup(&pp, &d, &s, &out);
}

/* A header that cannot be found is one diagnostic, not a crash. */
static void test_not_found(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <does_not_exist_qcc.h>\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "one not-found error");
    cleanup(&pp, &d, &s, &out);
}

/* #include with no filename is a diagnostic. */
static void test_missing_filename(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "missing filename diagnosed");
    cleanup(&pp, &d, &s, &out);
}

/* A self-including header terminates at the recursion cap with one diagnostic. */
static void test_depth_limit(void)
{
    qcc_pp pp; qcc_diag_sink d; qcc_source s; qcc_ptok_list out;
    QTEST_CHECK_EQ_INT(run_inc("#include <selfinc.h>\n",
                               QCC_INC_ANGLE_DIR, NULL, &pp, &d, &s, &out),
                       QCC_OK, "run terminates");
    QTEST_CHECK_EQ_UINT(errors(&d), 1, "one too-deep error");
    cleanup(&pp, &d, &s, &out);
}

int main(void)
{
    test_angle_found();
    test_quote_found();
    test_quote_falls_back_to_angle();
    test_angle_ignores_quote_dir();
    test_nested_include();
    test_subdir_include();
    test_file_line_in_include();
    test_computed_include_string();
    test_computed_include_angle();
    test_not_found();
    test_missing_filename();
    test_depth_limit();
    return qtest_report("include");
}
