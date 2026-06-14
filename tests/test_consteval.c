/*
 * Tests for the integer constant-expression evaluator (ISO C11 §6.6; ADR-0025):
 * the arithmetic core, the integer promotions (§6.3.1.1) and usual arithmetic
 * conversions (§6.3.1.8) on LP64 (signedness/width affecting /, %, comparisons,
 * and wrap-around), integer casts, sizeof/_Alignof of a type-name, short-circuit
 * evaluation, and the forms that are not integer constant expressions. Each case
 * parses one expression through the full front end (with a type context, so casts
 * and sizeof work) and evaluates the tree.
 */
#include "qtest.h"

#include <stdint.h>
#include <string.h>

#include "ast/ast.h"
#include "consteval/consteval.h"
#include "convert/convert.h"
#include "diag/diag.h"
#include "parser/parser.h"
#include "pp/pp.h"
#include "source/source.h"
#include "symtab/symtab.h"
#include "type/type.h"

/* Parse one expression from `text` and evaluate it as an integer constant
   expression. Returns the eval status; *out holds the value on QCC_OK. */
static qcc_status eval_text(const char *text, qcc_const_value *out)
{
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

    qcc_expr   *e  = NULL;
    qcc_status  st = qcc_parse_expression(&parser, &e);
    qcc_status  ev = (st == QCC_OK && e != NULL) ? qcc_eval_const_int(e, out)
                                                 : QCC_ERR_PARSE;

    qcc_ast_dispose(&ast);
    qcc_symtab_dispose(&sym);
    qcc_type_ctx_dispose(&tc);
    qcc_token_list_dispose(&toks);
    qcc_convert_dispose(&cv);
    qcc_ptok_list_dispose(&pt);
    qcc_pp_dispose(&pp);
    qcc_source_dispose(&s);
    qcc_diag_sink_dispose(&d);
    return ev;
}

/* Assert `text` evaluates to the signed value `val` with type `ty`. */
static void chk_s(const char *text, int64_t val, qcc_int_type ty)
{
    qcc_const_value cv;
    memset(&cv, 0, sizeof(cv));
    QTEST_CHECK_EQ_INT(eval_text(text, &cv), QCC_OK, text);
    QTEST_CHECK_EQ_INT((int64_t)cv.value, val, text);
    QTEST_CHECK_EQ_INT((int)cv.type, (int)ty, text);
}

/* Assert `text` evaluates to the unsigned value `val` with type `ty`. */
static void chk_u(const char *text, uint64_t val, qcc_int_type ty)
{
    qcc_const_value cv;
    memset(&cv, 0, sizeof(cv));
    QTEST_CHECK_EQ_INT(eval_text(text, &cv), QCC_OK, text);
    QTEST_CHECK_EQ_UINT(cv.value, val, text);
    QTEST_CHECK_EQ_INT((int)cv.type, (int)ty, text);
}

/* Assert `text` is not an integer constant expression. */
static void chk_err(const char *text)
{
    qcc_const_value cv;
    memset(&cv, 0, sizeof(cv));
    QTEST_CHECK_TRUE(eval_text(text, &cv) != QCC_OK);
}

static void test_arithmetic(void)
{
    chk_s("1 + 2", 3, QCC_INT_INT);
    chk_s("2 * 3 + 4", 10, QCC_INT_INT);
    chk_s("2 + 3 * 4", 14, QCC_INT_INT);
    chk_s("17 / 5", 3, QCC_INT_INT);
    chk_s("17 % 5", 2, QCC_INT_INT);
    chk_s("1 << 4", 16, QCC_INT_INT);
    chk_s("100 >> 2", 25, QCC_INT_INT);
    chk_s("7 & 3", 3, QCC_INT_INT);
    chk_s("5 | 2", 7, QCC_INT_INT);
    chk_s("5 ^ 1", 4, QCC_INT_INT);
    chk_s("~0", -1, QCC_INT_INT);
    chk_s("-5", -5, QCC_INT_INT);
    chk_s("-(2 + 3)", -5, QCC_INT_INT);
    chk_s("'A'", 65, QCC_INT_INT);
    chk_s("'A' + 1", 66, QCC_INT_INT);
}

static void test_logical_relational(void)
{
    chk_s("!0", 1, QCC_INT_INT);
    chk_s("!5", 0, QCC_INT_INT);
    chk_s("3 > 2", 1, QCC_INT_INT);
    chk_s("2 >= 2", 1, QCC_INT_INT);
    chk_s("2 == 3", 0, QCC_INT_INT);
    chk_s("2 != 3", 1, QCC_INT_INT);
    chk_s("1 && 1", 1, QCC_INT_INT);
    chk_s("1 && 0", 0, QCC_INT_INT);
    chk_s("0 || 0", 0, QCC_INT_INT);
    chk_s("1 ? 10 : 20", 10, QCC_INT_INT);
    chk_s("0 ? 10 : 20", 20, QCC_INT_INT);
    chk_s("(2 > 1) ? 5 : 6", 5, QCC_INT_INT);
}

static void test_unsigned_and_width(void)
{
    chk_u("5u + 3", 8, QCC_INT_UINT);
    chk_u("0xFFFFFFFFu", 4294967295u, QCC_INT_UINT);
    chk_u("1u - 2u", 4294967295u, QCC_INT_UINT); /* unsigned wrap-around */
    chk_s("1 - 2", -1, QCC_INT_INT);
    chk_u("1u - 2", 4294967295u, QCC_INT_UINT);  /* uint vs int -> uint */
    chk_s("1 + 1L", 2, QCC_INT_LONG);            /* int vs long -> long  */
    chk_s("1L << 40", 1099511627776LL, QCC_INT_LONG);
    chk_u("18446744073709551615ULL", 18446744073709551615ULL, QCC_INT_ULONG);
    /* §6.3.1.8 gotcha: 1u < -1 converts -1 to UINT_MAX, so the comparison is true. */
    chk_s("1u < -1", 1, QCC_INT_INT);
    chk_s("-1 < 1", 1, QCC_INT_INT);
}

static void test_casts(void)
{
    chk_s("(int)5", 5, QCC_INT_INT);
    chk_s("(long)5", 5, QCC_INT_LONG);
    chk_s("(char)300", 44, QCC_INT_INT);     /* 300 & 0xFF = 44 */
    chk_s("(char)200", -56, QCC_INT_INT);    /* sign bit set -> -56 */
    chk_s("(short)70000", 4464, QCC_INT_INT);
    chk_s("(unsigned char)300", 44, QCC_INT_INT);
    chk_u("(unsigned)-1", 4294967295u, QCC_INT_UINT);
    chk_s("(_Bool)5", 1, QCC_INT_INT);
    chk_s("(_Bool)0", 0, QCC_INT_INT);
}

static void test_sizeof_alignof(void)
{
    chk_u("sizeof(int)", 4, QCC_INT_ULONG);
    chk_u("sizeof(char)", 1, QCC_INT_ULONG);
    chk_u("sizeof(double)", 8, QCC_INT_ULONG);
    chk_u("sizeof(int *)", 8, QCC_INT_ULONG);
    chk_u("sizeof(int[4])", 16, QCC_INT_ULONG);
    chk_u("_Alignof(double)", 8, QCC_INT_ULONG);
    chk_u("sizeof(int) + 1", 5, QCC_INT_ULONG); /* size_t + int -> size_t */
}

static void test_short_circuit(void)
{
    chk_s("1 || (1 / 0)", 1, QCC_INT_INT); /* right operand not evaluated */
    chk_s("0 && (1 / 0)", 0, QCC_INT_INT);
    chk_s("1 ? 5 : (1 / 0)", 5, QCC_INT_INT);
}

static void test_not_constant(void)
{
    chk_err("x");          /* an identifier is not (yet) a constant */
    chk_err("x + 1");
    chk_err("1.5");        /* floating constant */
    chk_err("1 + 1.5");    /* floating operand */
    chk_err("\"s\"");      /* string literal */
    chk_err("f()");        /* call */
    chk_err("a, b");       /* comma operator */
    chk_err("sizeof x");   /* sizeof of an expression needs its type */
    chk_err("10 / 0");     /* division by zero */
    chk_err("10 % 0");
}

static void test_invalid_args(void)
{
    qcc_const_value cv;
    QTEST_CHECK_EQ_INT(qcc_eval_const_int(NULL, &cv), QCC_ERR_INVALID_ARGUMENT,
                       "null expr");
    /* A throwaway leaf so we can pass a non-NULL expr with a NULL out. */
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind      = QCC_TOKEN_INTEGER;
    tok.int_value = 1;
    tok.int_type  = QCC_INT_INT;
    qcc_expr *e = qcc_expr_leaf(&ast, &tok);
    QTEST_CHECK_EQ_INT(qcc_eval_const_int(e, NULL), QCC_ERR_INVALID_ARGUMENT,
                       "null out");
    qcc_ast_dispose(&ast);
}

int main(void)
{
    test_arithmetic();
    test_logical_relational();
    test_unsigned_and_width();
    test_casts();
    test_sizeof_alignof();
    test_short_circuit();
    test_not_constant();
    test_invalid_args();
    return qtest_report("consteval");
}
