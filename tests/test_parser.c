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
#include "symtab/symtab.h"
#include "type/type.h"

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
    qcc_parser_init(&parser, toks.items, toks.count, &ast, NULL, NULL, &d);

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

/*
 * Like parse_expr but with a type context and symbol table, so the type-dependent
 * §6.5 forms (cast, sizeof(type-name), _Alignof) are exercised. Any leading
 * declarations (e.g. a `typedef`) are parsed first so their names register, then
 * exactly one expression is parsed and its tree dumped. *errs gets the diagnostic
 * error count.
 */
static qcc_status parse_expr_typed(const char *text, char **dump, size_t *errs)
{
    *dump = NULL;
    if (errs != NULL) {
        *errs = 0;
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

    qcc_decl_list decls;
    qcc_decl_list_init(&decls);
    qcc_status st = QCC_OK;
    while (!qcc_parser_at_end(&parser) && qcc_parser_at_declaration(&parser)) {
        st = qcc_parse_declaration(&parser, &decls);
        if (st != QCC_OK) {
            break;
        }
    }
    if (st == QCC_OK) {
        qcc_expr *e = NULL;
        st          = qcc_parse_expression(&parser, &e);
        if (st == QCC_OK && e != NULL) {
            size_t len = 0;
            qcc_expr_dump(e, dump, &len);
        }
    }
    if (errs != NULL) {
        *errs = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);
    }

    qcc_decl_list_dispose(&decls);
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

/* Parse `text` in a type context and assert its dump equals `expected`. */
static void chk_typed(const char *text, const char *expected)
{
    char  *dump = NULL;
    size_t errs = 0;
    qcc_status st = parse_expr_typed(text, &dump, &errs);
    QTEST_CHECK_EQ_INT(st, QCC_OK, text);
    QTEST_CHECK_EQ_UINT(errs, 0, "no errors");
    if (dump != NULL) {
        QTEST_CHECK_SPAN(dump, strlen(dump), expected, text);
        free(dump);
    } else {
        QTEST_CHECK_TRUE(0);
    }
}

/* Parse `text` in a type context and assert it is a syntax error. */
static void chk_typed_error(const char *text)
{
    char  *dump = NULL;
    size_t errs = 0;
    qcc_status st = parse_expr_typed(text, &dump, &errs);
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

/* The §6.5 forms that need a type context (ADR-0022): cast (§6.5.4), and
   sizeof/_Alignof of a type-name (§6.5.3.4). */
static void test_cast(void)
{
    chk_typed("(int)x", "(cast int x)");
    chk_typed("(unsigned long)0", "(cast unsigned long 0)");
    chk_typed("(int *)p", "(cast pointer to int p)");
    chk_typed("(char)(int)x", "(cast char (cast int x))");  /* right-assoc */
    chk_typed("(int)x * y", "(* (cast int x) y)");          /* cast binds tighter */
    chk_typed("-(int)x", "(- (cast int x))");               /* §6.5.3: unary op of a cast */
    /* A typedef-name names a type, so (T) is a cast; a plain name is not. */
    chk_typed("typedef int T; (T)x", "(cast int x)");
    chk_typed("typedef int *PI; (PI)x", "(cast pointer to int x)");
    /* The typedef/keyword test decides cast vs. parenthesised expression:
       `(int)+y` casts the unary `+y`, but `(x)+y` (x is no type) is `x + y`. */
    chk_typed("(int) + y", "(cast int (+ y))");
    chk_typed("(x) + y", "(+ x y)");
}

static void test_sizeof_type(void)
{
    chk_typed("sizeof(int)", "(sizeof-type int)");
    chk_typed("sizeof(char *)", "(sizeof-type pointer to char)");
    chk_typed("sizeof(int[4])", "(sizeof-type array[4] of int)");
    chk_typed("sizeof(int) + 1", "(+ (sizeof-type int) 1)"); /* sizeof binds tight */
    /* Without a type-name, sizeof still takes an expression. */
    chk_typed("sizeof x", "(sizeof x)");
    chk_typed("sizeof(x)", "(sizeof x)");          /* x is not a type name */
    chk_typed("sizeof(a + b)", "(sizeof (+ a b))");
    chk_typed("typedef int T; sizeof(T)", "(sizeof-type int)");
}

static void test_alignof(void)
{
    chk_typed("_Alignof(int)", "(alignof-type int)");
    chk_typed("_Alignof(double)", "(alignof-type double)");
    chk_typed("typedef long L; _Alignof(L)", "(alignof-type long)");
    chk_typed_error("_Alignof x");   /* _Alignof requires ( type-name ) */
}

static void test_type_form_errors(void)
{
    chk_typed_error("(int)");        /* a cast with no operand */
    chk_typed_error("(int){0}");     /* compound literal: not supported yet */
    chk_typed_error("sizeof(int");   /* missing ')' */
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
    QTEST_CHECK_EQ_INT(qcc_parser_init(NULL, &tok, 1, &ast, NULL, NULL, &d),
                       QCC_ERR_INVALID_ARGUMENT, "null parser");
    QTEST_CHECK_EQ_INT(qcc_parser_init(&pr, &tok, 0, &ast, NULL, NULL, &d),
                       QCC_ERR_INVALID_ARGUMENT, "zero count");
    QTEST_CHECK_EQ_INT(qcc_parser_init(&pr, &tok, 1, &ast, NULL, NULL, &d), QCC_OK,
                       "ok init");
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
    test_cast();
    test_sizeof_type();
    test_alignof();
    test_type_form_errors();
    test_from_macro();
    test_errors();
    test_cursor_and_args();
    return qtest_report("parser");
}
