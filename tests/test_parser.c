/*
 * Tests for the parser (ISO C11 §6.5; ADR-0019 Unit 1): the expression grammar,
 * driven through the real front end (source -> preprocess -> convert -> parse) so
 * the parser is exercised on genuine token streams. Each case parses one
 * expression and compares the S-expression dump of the tree, which makes operator
 * precedence and associativity directly checkable.
 */
#include "qtest.h"

#include <stdlib.h>
#include <string.h>

#include "ast/ast.h"
#include "convert/convert.h"
#include "diag/diag.h"
#include "parser/parser.h"
#include "pp/pp.h"
#include "source/source.h"

/*
 * Run the whole front end on `text`, parse one expression, and return its dump
 * (heap-owned, caller frees — produced before teardown so it outlives the arena).
 * *errs gets the diagnostic error count, *at_end whether the cursor reached EOF.
 */
static qcc_status parse_expr(const char *text, char **dump, size_t *errs,
                             int *at_end)
{
    *dump = NULL;
    if (errs != NULL) {
        *errs = 0;
    }
    if (at_end != NULL) {
        *at_end = 0;
    }

    qcc_diag_sink d;
    qcc_diag_sink_init(&d);

    qcc_source s;
    QTEST_CHECK_EQ_INT(qcc_source_from_memory("t.c", text, strlen(text), &s),
                       QCC_OK, "source");

    qcc_pp pp;
    qcc_pp_init(&pp, &d);
    qcc_ptok_list pt;
    qcc_ptok_list_init(&pt);
    qcc_pp_run(&pp, &s, &pt);

    qcc_convert cv;
    qcc_convert_init(&cv, &d);
    qcc_token_list toks;
    qcc_token_list_init(&toks);
    qcc_convert_run(&cv, &pt, &toks);

    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_parser parser;
    qcc_parser_init(&parser, toks.items, toks.count, &ast, &d);

    qcc_expr  *e  = NULL;
    qcc_status st = qcc_parse_expression(&parser, &e);
    if (st == QCC_OK && e != NULL) {
        size_t len = 0;
        qcc_expr_dump(e, dump, &len);
    }
    if (errs != NULL) {
        *errs = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);
    }
    if (at_end != NULL) {
        *at_end = qcc_parser_at_end(&parser);
    }

    qcc_ast_dispose(&ast);
    qcc_token_list_dispose(&toks);
    qcc_convert_dispose(&cv);
    qcc_ptok_list_dispose(&pt);
    qcc_pp_dispose(&pp);
    qcc_source_dispose(&s);
    qcc_diag_sink_dispose(&d);
    return st;
}

/* Parse `text` and assert its dump equals `expected` with no errors. */
static void chk(const char *text, const char *expected)
{
    char  *dump = NULL;
    size_t errs = 0;
    qcc_status st = parse_expr(text, &dump, &errs, NULL);
    QTEST_CHECK_EQ_INT(st, QCC_OK, text);
    QTEST_CHECK_EQ_UINT(errs, 0, "no errors");
    if (dump != NULL) {
        QTEST_CHECK_SPAN(dump, strlen(dump), expected, text);
        free(dump);
    } else {
        QTEST_CHECK_TRUE(0);
    }
}

/* Parse `text` and assert it is a syntax error. */
static void chk_error(const char *text)
{
    char  *dump = NULL;
    size_t errs = 0;
    qcc_status st = parse_expr(text, &dump, &errs, NULL);
    QTEST_CHECK_TRUE(st != QCC_OK);
    QTEST_CHECK_TRUE(errs >= 1);
    free(dump);
}

static void test_primary_and_leaves(void)
{
    chk("x", "x");
    chk("42", "42");
    chk("1.5", "1.5");
    chk("'A'", "(char 65)");
    chk("\"hi\"", "(str)");
    chk("(a)", "a"); /* parentheses are transparent */
}

static void test_precedence(void)
{
    chk("a + b * c", "(+ a (* b c))");
    chk("a * b + c", "(+ (* a b) c)");
    chk("1 << 2 + 3", "(<< 1 (+ 2 3))");       /* + binds tighter than << */
    chk("a == b != c", "(!= (== a b) c)");      /* level 6, left-assoc */
    chk("a & b | c", "(| (& a b) c)");          /* & (5) tighter than | (3) */
    chk("a && b || c", "(|| (&& a b) c)");
    chk("a < b == c", "(== (< a b) c)");        /* relational tighter than == */
}

static void test_associativity(void)
{
    chk("a - b - c", "(- (- a b) c)");          /* binary: left-assoc */
    chk("a = b = c", "(= a (= b c))");          /* assignment: right-assoc */
    chk("a ? b : c ? d : e", "(?: a b (?: c d e))"); /* ?: right-assoc */
    chk("a, b, c", "(, (, a b) c)");            /* comma: left-assoc */
}

static void test_unary_and_postfix(void)
{
    chk("-a + +b", "(+ (- a) (+ b))");
    chk("!a && b", "(&& (! a) b)");
    chk("*p = 5", "(= (* p) 5)");               /* deref then assign */
    chk("&x", "(& x)");
    chk("~m", "(~ m)");
    chk("++a", "(pre++ a)");
    chk("a++", "(post++ a)");
    chk("++a--", "(pre++ (post-- a))");         /* postfix binds tighter */
    chk("x += 1", "(+= x 1)");
}

static void test_postfix_chains(void)
{
    chk("a[i]", "([] a i)");
    chk("a[i][j]", "([] ([] a i) j)");
    chk("f(x, y + 1)", "(call f x (+ y 1))");
    chk("f()", "(call f)");
    chk("p->x.y", "(. (-> p x) y)");
    chk("g(a)(b)", "(call (call g a) b)");      /* call result is callable */
}

static void test_conditional_middle(void)
{
    /* The middle of ?: is a full expression, so a comma is allowed there. */
    chk("a ? b, c : d", "(?: a (, b c) d)");
    chk("a ? b : c", "(?: a b c)");
}

static void test_sizeof(void)
{
    chk("sizeof x + 1", "(+ (sizeof x) 1)");    /* sizeof is a unary operator */
    chk("sizeof(a + b)", "(sizeof (+ a b))");
}

static void test_from_macro(void)
{
    /* Parsing happens after preprocessing, so a macro feeds the parser. */
    chk("#define N 2\nN * 3", "(* 2 3)");
}

static void test_errors(void)
{
    chk_error("a +");          /* missing right operand */
    chk_error("(a");           /* missing ')' */
    chk_error("f(a,");         /* missing argument */
    chk_error("a ? b");        /* missing ':' */
    chk_error("a .");          /* missing member name */
    chk_error("");             /* empty: expected an expression */
    chk_error("* ");           /* '*' with no operand */
}

static void test_cursor_and_args(void)
{
    /* One expression is parsed; "a b" leaves the cursor on 'b' (not EOF). */
    char  *dump   = NULL;
    int    at_end = 1;
    QTEST_CHECK_EQ_INT(parse_expr("a b", &dump, NULL, &at_end), QCC_OK, "parse a");
    QTEST_CHECK_TRUE(!at_end);
    free(dump);

    dump = NULL;
    QTEST_CHECK_EQ_INT(parse_expr("a + b", &dump, NULL, &at_end), QCC_OK, "parse");
    QTEST_CHECK_TRUE(at_end);
    free(dump);

    /* Argument validation. */
    qcc_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind = QCC_TOKEN_EOF;
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_parser pr;
    QTEST_CHECK_EQ_INT(qcc_parser_init(NULL, &tok, 1, &ast, &d),
                       QCC_ERR_INVALID_ARGUMENT, "null parser");
    QTEST_CHECK_EQ_INT(qcc_parser_init(&pr, &tok, 0, &ast, &d),
                       QCC_ERR_INVALID_ARGUMENT, "zero count");
    QTEST_CHECK_EQ_INT(qcc_parser_init(&pr, &tok, 1, &ast, &d), QCC_OK, "ok init");
    qcc_diag_sink_dispose(&d);
    qcc_ast_dispose(&ast);
}

int main(void)
{
    test_primary_and_leaves();
    test_precedence();
    test_associativity();
    test_unary_and_postfix();
    test_postfix_chains();
    test_conditional_middle();
    test_sizeof();
    test_from_macro();
    test_errors();
    test_cursor_and_args();
    return qtest_report("parser");
}
