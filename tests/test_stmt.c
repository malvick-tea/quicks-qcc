/*
 * Tests for the statement parser (ISO C11 §6.8; ADR-0023 / parser Unit 3):
 * compound statements and block scoping (§6.8.2), expression and null statements
 * (§6.8.3), selection (§6.8.4), iteration (§6.8.5), jump (§6.8.6) and labeled
 * (§6.8.1) statements, the dangling-else rule, and the declaration-vs-statement
 * test at each block item (incl. a typedef introduced mid-block). Each statement
 * is parsed through the full front end and its S-expression dump compared.
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
#include "symtab/symtab.h"
#include "type/type.h"

/*
 * Run the front end on `text`, parse one statement, and return its dump (heap,
 * caller frees — produced before teardown so it outlives the arena). *errs gets
 * the diagnostic error count, *at_end whether the cursor reached EOF.
 */
static qcc_status parse_stmt(const char *text, char **dump, size_t *errs,
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

    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab sym;
    qcc_symtab_init(&sym);
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_parser parser;
    qcc_parser_init(&parser, toks.items, toks.count, &ast, &tc, &sym, &d);

    qcc_stmt  *node = NULL;
    qcc_status st   = qcc_parse_statement(&parser, &node);
    if (st == QCC_OK && node != NULL) {
        size_t len = 0;
        qcc_stmt_dump(node, dump, &len);
    }
    if (errs != NULL) {
        *errs = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);
    }
    if (at_end != NULL) {
        *at_end = qcc_parser_at_end(&parser);
    }

    qcc_ast_dispose(&ast);
    qcc_symtab_dispose(&sym);
    qcc_type_ctx_dispose(&tc);
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
    qcc_status st = parse_stmt(text, &dump, &errs, NULL);
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
    qcc_status st = parse_stmt(text, &dump, &errs, NULL);
    QTEST_CHECK_TRUE(st != QCC_OK);
    QTEST_CHECK_TRUE(errs >= 1);
    free(dump);
}

static void test_expression_and_null(void)
{
    chk(";", "(empty)");                  /* the null statement (§6.8.3) */
    chk("x;", "(expr x)");
    chk("x = 1;", "(expr (= x 1))");
    chk("f();", "(expr (call f))");
    chk("a + b * c;", "(expr (+ a (* b c)))");
}

static void test_compound(void)
{
    chk("{}", "(block)");
    chk("{ ; }", "(block (empty))");
    chk("{ x; y; }", "(block (expr x) (expr y))");
    chk("{ { x; } }", "(block (block (expr x)))"); /* nested block */
    chk("{ int a; a = 1; }", "(block (decl a) (expr (= a 1)))");
    chk("{ int a, b; }", "(block (decl a b))"); /* one declaration, two decls */
}

static void test_if(void)
{
    chk("if (c) x;", "(if c (expr x))");
    chk("if (c) x; else y;", "(if c (expr x) (expr y))");
    /* The else binds to the nearest if (§6.8.4.1). */
    chk("if (a) if (b) x; else y;", "(if a (if b (expr x) (expr y)))");
    chk("if (a) { x; } else { y; }",
        "(if a (block (expr x)) (block (expr y)))");
}

static void test_iteration(void)
{
    chk("while (c) x;", "(while c (expr x))");
    chk("while (c) ;", "(while c (empty))");
    chk("do x; while (c);", "(do (expr x) c)");
    chk("for (i = 0; i < n; ++i) x;",
        "(for (expr (= i 0)) (< i n) (pre++ i) (expr x))");
    chk("for (;;) ;", "(for nil nil nil (empty))");
    chk("for (; c; ) body();", "(for nil c nil (expr (call body)))");
    /* A declaration in clause 1 is scoped to the loop (§6.8.5.3). */
    chk("for (int i = 0; i < n; i++) ;",
        "(for (decl i) (< i n) (post++ i) (empty))");
}

static void test_switch(void)
{
    chk("switch (x) { case 1: a; case 2: b; default: c; }",
        "(switch x (block (case 1 (expr a)) (case 2 (expr b)) "
        "(default (expr c))))");
    chk("switch (x) { default: ; }", "(switch x (block (default (empty))))");
}

static void test_jump(void)
{
    chk("return;", "(return)");
    chk("return x + 1;", "(return (+ x 1))");
    chk("break;", "(break)");
    chk("continue;", "(continue)");
    chk("goto done;", "(goto done)");
}

static void test_labeled(void)
{
    chk("foo: x;", "(label foo (expr x))");
    chk("end: ;", "(label end (empty))");
    chk("{ goto skip; skip: x; }",
        "(block (goto skip) (label skip (expr x)))");
}

static void test_block_scope_and_typedef(void)
{
    /* A typedef introduced in a block is honored by later items (§6.7.8). */
    chk("{ typedef int T; T y; }", "(block (decl T) (decl y))");
    /* Inner declaration shadows the outer; both parse in their own scope. */
    chk("{ int x; { int x; } }", "(block (decl x) (block (decl x)))");
    /* A label may spell a typedef-name (labels are a separate name space). */
    chk("{ typedef int T; T: x; }", "(block (decl T) (label T (expr x)))");
}

static void test_errors(void)
{
    chk_error("if x;");        /* missing '(' after if */
    chk_error("if (c)");       /* missing substatement */
    chk_error("{ x }");        /* missing ';' after expression */
    chk_error("return");       /* missing ';' */
    chk_error("{");            /* unclosed block */
    chk_error("case 1 x;");    /* missing ':' after case label */
    chk_error("do x; y;");     /* missing 'while' after do body */
    chk_error("goto ;");       /* missing label name */
}

static void test_cursor_and_args(void)
{
    /* One statement is parsed; "x; y;" leaves the cursor on 'y' (not EOF). */
    char *dump   = NULL;
    int   at_end = 1;
    QTEST_CHECK_EQ_INT(parse_stmt("x; y;", &dump, NULL, &at_end), QCC_OK, "one");
    QTEST_CHECK_TRUE(!at_end);
    free(dump);

    dump = NULL;
    QTEST_CHECK_EQ_INT(parse_stmt("{ x; }", &dump, NULL, &at_end), QCC_OK, "block");
    QTEST_CHECK_TRUE(at_end);
    free(dump);

    /* Statements need a type context and symbol table. */
    qcc_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind = QCC_TOKEN_EOF;
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_parser pr;
    qcc_parser_init(&pr, &tok, 1, &ast, NULL, NULL, &d);
    qcc_stmt *out = NULL;
    QTEST_CHECK_EQ_INT(qcc_parse_statement(&pr, &out), QCC_ERR_INVALID_ARGUMENT,
                       "needs types/syms");
    qcc_diag_sink_dispose(&d);
    qcc_ast_dispose(&ast);
}

int main(void)
{
    test_expression_and_null();
    test_compound();
    test_if();
    test_iteration();
    test_switch();
    test_jump();
    test_labeled();
    test_block_scope_and_typedef();
    test_errors();
    test_cursor_and_args();
    return qtest_report("stmt");
}
