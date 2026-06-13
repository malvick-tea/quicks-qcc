/*
 * Tests for the preprocessor's public surface at the materialization stage:
 * qcc_pp_run turns the lexer's phase-1-3 stream into qcc_ptok values with
 * interned spellings, recorded provenance, and an empty hide set. (Directive
 * and macro behavior get their own cases as those stages land.)
 */
#include "qtest.h"

#include <string.h>

#include "diag/diag.h"
#include "pp/pp.h"
#include "source/source.h"
#include "token/token.h"

/* Run the preprocessor over `text`; fills *out (caller disposes out, pp, diags,
   src). Returns the qcc_pp_run status. */
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

/* The stream is materialized with kinds, spellings, and a trailing EOF. */
static void test_materialize_stream(void)
{
    qcc_pp        pp;
    qcc_diag_sink diags;
    qcc_source    src;
    qcc_ptok_list out;
    qcc_status    st = run("int x = 42;\n", &pp, &diags, &src, &out);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "run ok");

    /* int, x, =, 42, ;, NEWLINE, EOF */
    QTEST_CHECK_EQ_UINT(out.count, 7, "token count");
    QTEST_CHECK_EQ_INT(out.items[0].kind, QCC_PP_TOKEN_IDENTIFIER, "int kind");
    QTEST_CHECK_SPAN(out.items[0].spelling, out.items[0].spelling_len, "int", "int");
    QTEST_CHECK_EQ_INT(out.items[1].kind, QCC_PP_TOKEN_IDENTIFIER, "x kind");
    QTEST_CHECK_SPAN(out.items[1].spelling, out.items[1].spelling_len, "x", "x");
    QTEST_CHECK_EQ_INT(out.items[2].kind, QCC_PP_TOKEN_PUNCT, "= kind");
    QTEST_CHECK_EQ_INT(out.items[2].punct, QCC_PUNCT_EQ, "= punct");
    QTEST_CHECK_EQ_INT(out.items[3].kind, QCC_PP_TOKEN_PP_NUMBER, "42 kind");
    QTEST_CHECK_SPAN(out.items[3].spelling, out.items[3].spelling_len, "42", "42");
    QTEST_CHECK_EQ_INT(out.items[4].kind, QCC_PP_TOKEN_PUNCT, "; kind");
    QTEST_CHECK_EQ_INT(out.items[5].kind, QCC_PP_TOKEN_NEWLINE, "newline");
    QTEST_CHECK_EQ_INT(out.items[6].kind, QCC_PP_TOKEN_EOF, "eof");

    /* The first token starts its line; '=' has surrounding whitespace. */
    QTEST_CHECK_TRUE(out.items[0].at_line_start);
    QTEST_CHECK_TRUE(out.items[2].leading_space);

    /* Provenance is recorded: line 1 for everything on the first line. */
    QTEST_CHECK_EQ_INT(out.items[0].line, 1, "int on line 1");
    QTEST_CHECK_TRUE(out.items[0].source == &src);

    /* Every token starts with an empty hide set. */
    QTEST_CHECK_TRUE(out.items[0].hideset == NULL);

    cleanup(&pp, &diags, &src, &out);
}

/* Spellings are interned: equal spellings share a pointer across the stream. */
static void test_interned_spellings(void)
{
    qcc_pp        pp;
    qcc_diag_sink diags;
    qcc_source    src;
    qcc_ptok_list out;
    qcc_status    st = run("foo foo bar foo\n", &pp, &diags, &src, &out);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "run ok");

    /* foo foo bar foo NEWLINE EOF */
    QTEST_CHECK_EQ_UINT(out.count, 6, "token count");
    QTEST_CHECK_TRUE(out.items[0].spelling == out.items[1].spelling); /* foo==foo */
    QTEST_CHECK_TRUE(out.items[0].spelling == out.items[3].spelling); /* foo==foo */
    QTEST_CHECK_TRUE(out.items[0].spelling != out.items[2].spelling); /* foo!=bar */

    /* qcc_pp_intern yields the same canonical pointer. */
    const char *foo = qcc_pp_intern(&pp, "foo", 3);
    QTEST_CHECK_TRUE(foo == out.items[0].spelling);

    cleanup(&pp, &diags, &src, &out);
}

/* A long token (longer than the initial scratch buffer) materializes intact,
   exercising the scratch-grow path. */
static void test_long_token(void)
{
    char text[600];
    memset(text, 'a', sizeof(text));
    text[0]                = '"';            /* a 596-char string literal */
    text[sizeof(text) - 4] = '"';
    text[sizeof(text) - 3] = '\n';
    text[sizeof(text) - 2] = '\0';           /* terminate the C string early */

    qcc_pp        pp;
    qcc_diag_sink diags;
    qcc_source    src;
    qcc_ptok_list out;
    qcc_status    st = run(text, &pp, &diags, &src, &out);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "run ok");

    QTEST_CHECK_EQ_INT(out.items[0].kind, QCC_PP_TOKEN_STRING_LIT, "string kind");
    QTEST_CHECK_EQ_UINT(out.items[0].spelling_len, sizeof(text) - 3, "full length");

    cleanup(&pp, &diags, &src, &out);
}

/* Argument validation and NULL-safety. */
static void test_invalid_args(void)
{
    qcc_diag_sink diags;
    qcc_diag_sink_init(&diags);
    qcc_pp pp;
    QTEST_CHECK_EQ_INT(qcc_pp_init(NULL, &diags), QCC_ERR_INVALID_ARGUMENT, "null pp");
    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, NULL), QCC_ERR_INVALID_ARGUMENT, "null diags");

    QTEST_CHECK_EQ_INT(qcc_pp_init(&pp, &diags), QCC_OK, "init");
    qcc_ptok_list out;
    qcc_ptok_list_init(&out);
    QTEST_CHECK_EQ_INT(qcc_pp_run(&pp, NULL, &out), QCC_ERR_INVALID_ARGUMENT, "null src");

    qcc_ptok_list_dispose(&out);
    qcc_pp_dispose(&pp);
    qcc_pp_dispose(NULL); /* must not crash */
    qcc_diag_sink_dispose(&diags);
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_materialize_stream();
    test_interned_spellings();
    test_long_token();
    test_invalid_args();
    return qtest_report("pp");
}
