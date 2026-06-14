/*
 * Tests for the declaration parser (ISO C11 §6.7; ADR-0022 / parser Unit 2):
 * declaration-specifiers (unordered, §6.7.2), inside-out declarators (§6.7.6:
 * pointer/array/function, nested), typedef-name resolution and registration
 * (§6.7.8), and type-names. Each declaration is parsed through the full front end
 * and its declared (name, type) is formatted and compared.
 */
#include "qtest.h"

#include <stdio.h>
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

/* Append "name: [storage ]<type>" for one decl to `buf` (caller-sized). */
static void format_decl(char *buf, size_t cap, const qcc_decl *d)
{
    char *tp = NULL;
    size_t tl = 0;
    qcc_type_print(d->type, &tp, &tl);
    const char *sc = qcc_storage_class_name(d->storage);
    snprintf(buf, cap, "%.*s: %s%s%s", (int)d->name_len,
             d->name != NULL ? d->name : "", sc, (sc[0] != '\0') ? " " : "",
             tp != NULL ? tp : "?");
    free(tp);
}

/*
 * Parse every declaration in `text` and join their formatted forms with "; ".
 * Returns the joined dump (heap, caller frees) and the diagnostic error count.
 */
static void parse_decls(const char *text, char **out, size_t *errs)
{
    *out = NULL;
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
    qcc_symtab st;
    qcc_symtab_init(&st);
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_parser parser;
    qcc_parser_init(&parser, toks.items, toks.count, &ast, &tc, &st, &d);

    qcc_decl_list decls;
    qcc_decl_list_init(&decls);
    while (!qcc_parser_at_end(&parser) && qcc_parser_at_declaration(&parser)) {
        if (qcc_parse_declaration(&parser, &decls) != QCC_OK) {
            break;
        }
    }

    /* Build the joined dump before teardown (it copies the strings). */
    char line[256];
    char acc[1024];
    acc[0] = '\0';
    for (size_t i = 0; i < decls.count; ++i) {
        format_decl(line, sizeof(line), &decls.items[i]);
        if (i != 0) {
            strncat(acc, "; ", sizeof(acc) - strlen(acc) - 1);
        }
        strncat(acc, line, sizeof(acc) - strlen(acc) - 1);
    }
    size_t alen = strlen(acc);
    char  *copy = (char *)malloc(alen + 1);
    if (copy != NULL) {
        memcpy(copy, acc, alen + 1);
    }
    *out  = copy;
    *errs = qcc_diag_severity_count(&d, QCC_DIAG_ERROR);

    qcc_decl_list_dispose(&decls);
    qcc_ast_dispose(&ast);
    qcc_symtab_dispose(&st);
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
    parse_decls(text, &dump, &errs);
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
    parse_decls(text, &dump, &errs);
    QTEST_CHECK_TRUE(errs >= 1);
    free(dump);
}

static void test_basic_objects(void)
{
    chk("int x;", "x: int");
    chk("unsigned long y;", "y: unsigned long");
    chk("long long int z;", "z: long long");
    chk("const int c;", "c: const int");
    chk("static int s;", "s: static int");
    chk("unsigned u;", "u: unsigned int");
    chk("signed char b;", "b: signed char");
}

static void test_specifier_order(void)
{
    /* Specifiers are an unordered multiset (§6.7.2 ¶2). */
    chk("long unsigned int a;", "a: unsigned long");
    chk("int long unsigned b;", "b: unsigned long");
    chk("double long c;", "c: long double");
}

static void test_pointers_arrays(void)
{
    chk("int *p;", "p: pointer to int");
    chk("int **pp;", "pp: pointer to pointer to int");
    chk("char *const cp;", "cp: const pointer to char");
    chk("int a[3];", "a: array[3] of int");
    chk("int a[2][3];", "a: array[2] of array[3] of int");
    chk("char *argv[];", "argv: array[] of pointer to char");
    /* A bound is an integer constant expression (§6.7.6.2, §6.6), now evaluated. */
    chk("int a[2 + 3];", "a: array[5] of int");
    chk("int a[1 << 4];", "a: array[16] of int");
    chk("int a[sizeof(int)];", "a: array[4] of int");
}

static void test_functions(void)
{
    chk("int f(int, char);", "f: function(int, char) returning int");
    chk("void g(void);", "g: function() returning void");
    chk("int h(int a, ...);", "h: function(int, ...) returning int");
    /* int (*fp)(void): a pointer to a function (inside-out, §6.7.6). */
    chk("int (*fp)(void);", "fp: pointer to function() returning int");
    /* Array parameter adjusts to a pointer (§6.7.6.3). */
    chk("int k(int a[10]);", "k: function(pointer to int) returning int");
}

static void test_multiple_declarators(void)
{
    chk("int a, *b, c[3];",
        "a: int; b: pointer to int; c: array[3] of int");
}

static void test_typedef(void)
{
    chk("typedef int T;", "T: typedef int");
    /* A typedef-name is then usable as a type specifier (§6.7.8). */
    chk("typedef int T; T x;", "T: typedef int; x: int");
    chk("typedef int *IP; IP p;",
        "IP: typedef pointer to int; p: pointer to int");
    /* A later object declaration of the same name (different scope not needed at
       file scope here, but the kind is OBJECT). */
    chk("typedef unsigned long size_t; size_t n;",
        "size_t: typedef unsigned long; n: unsigned long");
}

static void test_struct_reference(void)
{
    /* A tag reference is an (incomplete) tagged type; definitions are deferred. */
    chk("struct point p;", "p: struct point");
    chk("struct node *next;", "next: pointer to struct node");
}

static void test_initializer(void)
{
    /* A scalar initializer parses as an assignment-expression. */
    chk("int x = 40 + 2;", "x: int");
    char  *dump = NULL;
    size_t errs = 0;
    parse_decls("int y = 5;", &dump, &errs);
    QTEST_CHECK_EQ_UINT(errs, 0, "init no error");
    free(dump);
}

static void test_errors(void)
{
    chk_error("int 3;");          /* not a declarator */
    chk_error("int *;");          /* missing identifier */
    chk_error("int f(int a");     /* missing ')' and ';' */
}

static void test_static_assert(void)
{
    /* A passing _Static_assert (§6.7.10) declares nothing — no decls, no error. */
    chk("_Static_assert(1, \"ok\");", "");
    chk("_Static_assert(sizeof(int) == 4, \"int is 4 bytes\");", "");
    chk("_Static_assert(2 + 2 == 4, \"math\");", "");
    /* It sits between ordinary declarations without contributing a declarator. */
    chk("int x; _Static_assert(1, \"k\"); int y;", "x: int; y: int");
    /* A false assertion, a non-constant expression, or a bad form is an error. */
    chk_error("_Static_assert(0, \"boom\");");
    chk_error("_Static_assert(2 > 3, \"no\");");
    chk_error("_Static_assert(n, \"not constant\");");
    chk_error("_Static_assert(1);"); /* missing message */
}

static void test_type_name_and_predicate(void)
{
    /* at_declaration distinguishes a declaration from an expression. */
    char  *dump = NULL;
    size_t errs = 0;
    parse_decls("x + 1;", &dump, &errs); /* not a declaration: nothing parsed */
    QTEST_CHECK_TRUE(dump != NULL && dump[0] == '\0');
    free(dump);
}

int main(void)
{
    test_basic_objects();
    test_specifier_order();
    test_pointers_arrays();
    test_functions();
    test_multiple_declarators();
    test_typedef();
    test_struct_reference();
    test_initializer();
    test_static_assert();
    test_errors();
    test_type_name_and_predicate();
    return qtest_report("decl");
}
