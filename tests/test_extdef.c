/*
 * Tests for the external-definition parser (ISO C11 §6.9; ADR-0024 / parser Unit
 * 4): function definitions vs. ordinary declarations, the parameter list bound in
 * the body's block scope (so a parameter shadowing a file-scope typedef-name flips
 * the §6.7.8 declaration-vs-statement test inside the body), and a whole
 * translation unit as the loop of external declarations. Each external declaration
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
 * Parse every external declaration in `text` and join their dumps with "; ".
 * Returns the joined dump (heap, caller frees) and the diagnostic error count.
 */
static void parse_extern(const char *text, char **out, size_t *errs)
{
    *out  = NULL;
    *errs = 0;

    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_source s;
    qcc_source_from_memory("t.c", text, strlen(text), &s);
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

    char acc[2048];
    acc[0]    = '\0';
    size_t ni = 0;
    while (!qcc_parser_at_end(&parser)) {
        qcc_extern_decl ed;
        if (qcc_parse_external_declaration(&parser, &ed) != QCC_OK) {
            break;
        }
        char  *one = NULL;
        size_t len = 0;
        if (qcc_extern_decl_dump(&ed, &one, &len) == QCC_OK && one != NULL) {
            if (ni != 0) {
                strncat(acc, "; ", sizeof(acc) - strlen(acc) - 1);
            }
            strncat(acc, one, sizeof(acc) - strlen(acc) - 1);
            ++ni;
        }
        free(one);
    }

    size_t alen = strlen(acc);
    char  *copy = (char *)malloc(alen + 1);
    if (copy != NULL) {
        memcpy(copy, acc, alen + 1);
    }
    *out  = copy;
    *errs = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);

    qcc_ast_dispose(&ast);
    qcc_symtab_dispose(&sym);
    qcc_type_ctx_dispose(&tc);
    qcc_token_list_dispose(&toks);
    qcc_convert_dispose(&cv);
    qcc_ptok_list_dispose(&pt);
    qcc_pp_dispose(&pp);
    qcc_source_dispose(&s);
    qcc_diag_sink_dispose(&d);
}

static void chk(const char *text, const char *expected)
{
    char  *dump = NULL;
    size_t errs = 0;
    parse_extern(text, &dump, &errs);
    QTEST_CHECK_EQ_UINT(errs, 0, text);
    if (dump != NULL) {
        QTEST_CHECK_SPAN(dump, strlen(dump), expected, text);
        free(dump);
    } else {
        QTEST_CHECK_TRUE(0);
    }
}

static void chk_error(const char *text)
{
    char  *dump = NULL;
    size_t errs = 0;
    parse_extern(text, &dump, &errs);
    QTEST_CHECK_TRUE(errs >= 1);
    free(dump);
}

static void test_declarations(void)
{
    chk("int x;", "(decl x)");
    chk("int a, b;", "(decl a b)");
    chk("struct S s;", "(decl s)");
    chk("typedef int T; T y;", "(decl T); (decl y)");
}

static void test_function_definitions(void)
{
    chk("int f(void) { return 0; }", "(func f () (block (return 0)))");
    chk("void g(void) {}", "(func g () (block))");
    chk("int add(int a, int b) { return a + b; }",
        "(func add (a b) (block (return (+ a b))))");
    chk("int f(int) { return 0; }", "(func f (?) (block (return 0)))"); /* unnamed */
}

static void test_translation_unit(void)
{
    /* A mix of declarations and definitions, in order. */
    chk("int x; int main(void) { return x; }",
        "(decl x); (func main () (block (return x)))");
    /* A prototype then its definition. */
    chk("int f(void); int f(void) { return 1; }",
        "(decl f); (func f () (block (return 1)))");
    /* The function name is in scope in its own body (recursion). */
    chk("int f(int n) { if (n) return f(n); return 0; }",
        "(func f (n) (block (if n (return (call f n))) (return 0)))");
}

static void test_parameter_scope(void)
{
    /* A parameter binds in the body scope: `T` is the parameter (an object), so
       `T * x;` is a multiplication statement, not a pointer declaration — the
       §6.7.8 test resolves the parameter that shadows the file-scope typedef. */
    chk("typedef int T; void f(int T) { T * x; }",
        "(decl T); (func f (T) (block (expr (* T x))))");
    /* Without a shadowing parameter, the typedef is visible in the body, so the
       same shape is a declaration. */
    chk("typedef int T; int g(void) { T x; return x; }",
        "(decl T); (func g () (block (decl x) (return x)))");
}

static void test_errors(void)
{
    chk_error("int 3;");              /* not a declarator */
    chk_error("int f(void) {");       /* unclosed function body */
    chk_error("int f(void) return;"); /* declarator not followed by '{' or ';' */
}

static void test_invalid_args(void)
{
    qcc_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind = QCC_TOKEN_EOF;
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_diag_sink d;
    qcc_diag_sink_init(&d);
    qcc_parser pr;
    qcc_parser_init(&pr, &tok, 1, &ast, NULL, NULL, &d);
    qcc_extern_decl ed;
    QTEST_CHECK_EQ_INT(qcc_parse_external_declaration(&pr, &ed),
                       QCC_ERR_INVALID_ARGUMENT, "needs types/syms");
    qcc_diag_sink_dispose(&d);
    qcc_ast_dispose(&ast);
}

int main(void)
{
    test_declarations();
    test_function_definitions();
    test_translation_unit();
    test_parameter_scope();
    test_errors();
    test_invalid_args();
    return qtest_report("extdef");
}
