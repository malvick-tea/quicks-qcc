/*
 * Tests for the ast module (ISO C11 §6.5; ADR-0019 Unit 1): the expression node
 * constructors and the S-expression dumper, exercised directly (the parser tests
 * exercise them through real source). Tokens are synthesized by hand here so the
 * module is tested without the whole front end.
 */
#include "qtest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ast/ast.h"
#include "token/token.h"
#include "type/type.h"

static qcc_token id_tok(const char *spelling)
{
    qcc_token t;
    memset(&t, 0, sizeof(t));
    t.kind         = QCC_TOKEN_IDENTIFIER;
    t.spelling     = spelling;
    t.spelling_len = strlen(spelling);
    return t;
}

static qcc_token int_tok(uint64_t value)
{
    qcc_token t;
    memset(&t, 0, sizeof(t));
    t.kind         = QCC_TOKEN_INTEGER;
    t.spelling     = "<int>";
    t.spelling_len = 5;
    t.int_value    = value;
    t.int_type     = QCC_INT_INT;
    return t;
}

/* Dump `e` and compare against `expected`, then free the dump. */
static void chk_dump(const qcc_expr *e, const char *expected, const char *what)
{
    QTEST_CHECK_TRUE(e != NULL);
    if (e == NULL) {
        return;
    }
    char  *dump = NULL;
    size_t len  = 0;
    QTEST_CHECK_EQ_INT(qcc_expr_dump(e, &dump, &len), QCC_OK, "dump ok");
    if (dump != NULL) {
        QTEST_CHECK_SPAN(dump, len, expected, what);
        free(dump);
    }
}

static void test_leaves(void)
{
    qcc_ast ast;
    QTEST_CHECK_EQ_INT(qcc_ast_init(&ast), QCC_OK, "init");

    qcc_token xi = id_tok("x");
    qcc_token n  = int_tok(42);
    chk_dump(qcc_expr_leaf(&ast, &xi), "x", "ident leaf");
    chk_dump(qcc_expr_leaf(&ast, &n), "42", "int leaf");

    /* A non-primary token is not a leaf. */
    qcc_token bad;
    memset(&bad, 0, sizeof(bad));
    bad.kind = QCC_TOKEN_PUNCT;
    QTEST_CHECK_TRUE(qcc_expr_leaf(&ast, &bad) == NULL);

    qcc_ast_dispose(&ast);
}

static void test_operators(void)
{
    qcc_ast ast;
    qcc_ast_init(&ast);

    qcc_token a = id_tok("a");
    qcc_token b = id_tok("b");
    qcc_token c = id_tok("c");
    qcc_token op;
    memset(&op, 0, sizeof(op));
    op.kind = QCC_TOKEN_PUNCT;

    qcc_expr *ea = qcc_expr_leaf(&ast, &a);
    qcc_expr *eb = qcc_expr_leaf(&ast, &b);
    qcc_expr *ec = qcc_expr_leaf(&ast, &c);

    chk_dump(qcc_expr_binary(&ast, QCC_PUNCT_PLUS, ea, eb, &op), "(+ a b)",
             "binary +");
    chk_dump(qcc_expr_unary(&ast, QCC_PUNCT_MINUS, ea, &op), "(- a)", "unary -");
    chk_dump(qcc_expr_unary(&ast, QCC_PUNCT_PLUS_PLUS, ea, &op), "(pre++ a)",
             "prefix ++");
    chk_dump(qcc_expr_postfix(&ast, QCC_PUNCT_MINUS_MINUS, ea, &op),
             "(post-- a)", "postfix --");
    chk_dump(qcc_expr_sizeof(&ast, ea, &op), "(sizeof a)", "sizeof");
    chk_dump(qcc_expr_assign(&ast, QCC_PUNCT_PLUS_EQ, ea, eb, &op), "(+= a b)",
             "assign +=");
    chk_dump(qcc_expr_conditional(&ast, ea, eb, ec, &op), "(?: a b c)",
             "conditional");
    chk_dump(qcc_expr_comma(&ast, ea, eb, &op), "(, a b)", "comma");
    chk_dump(qcc_expr_index(&ast, ea, eb, &op), "([] a b)", "index");
    chk_dump(qcc_expr_member(&ast, ea, 0, "m", 1, &op), "(. a m)", "member .");
    chk_dump(qcc_expr_member(&ast, ea, 1, "m", 1, &op), "(-> a m)", "member ->");

    qcc_expr *args[2] = { eb, ec };
    chk_dump(qcc_expr_call(&ast, ea, args, 2, &op), "(call a b c)", "call");
    chk_dump(qcc_expr_call(&ast, ea, NULL, 0, &op), "(call a)", "call no args");

    qcc_ast_dispose(&ast);
}

/* The type-dependent nodes (§6.5.3.4, §6.5.4) carry a qcc_type, rendered via
   qcc_type_print, and so need a type context to construct. */
static void test_type_operands(void)
{
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);

    const qcc_type *ti = qcc_type_basic(&tc, QCC_TYPE_INT);
    qcc_token       a  = id_tok("a");
    qcc_token       op;
    memset(&op, 0, sizeof(op));
    op.kind        = QCC_TOKEN_PUNCT;
    qcc_expr *ea   = qcc_expr_leaf(&ast, &a);

    chk_dump(qcc_expr_cast(&ast, ti, ea, &op), "(cast int a)", "cast");
    chk_dump(qcc_expr_sizeof_type(&ast, ti, &op), "(sizeof-type int)",
             "sizeof type");
    chk_dump(qcc_expr_alignof_type(&ast, ti, &op), "(alignof-type int)",
             "alignof type");

    /* A NULL type or operand yields NULL (mapped to QCC_ERR_OUT_OF_MEMORY/parse). */
    QTEST_CHECK_TRUE(qcc_expr_cast(&ast, NULL, ea, &op) == NULL);
    QTEST_CHECK_TRUE(qcc_expr_cast(&ast, ti, NULL, &op) == NULL);
    QTEST_CHECK_TRUE(qcc_expr_sizeof_type(&ast, NULL, &op) == NULL);
    QTEST_CHECK_TRUE(qcc_expr_alignof_type(&ast, NULL, &op) == NULL);

    qcc_type_ctx_dispose(&tc);
    qcc_ast_dispose(&ast);
}

static void test_kind_names(void)
{
    QTEST_CHECK_SPAN(qcc_expr_kind_name(QCC_EXPR_BINARY),
                     strlen(qcc_expr_kind_name(QCC_EXPR_BINARY)),
                     "binary", "binary name");
    QTEST_CHECK_SPAN(qcc_expr_kind_name(QCC_EXPR_CALL),
                     strlen(qcc_expr_kind_name(QCC_EXPR_CALL)),
                     "call", "call name");
    QTEST_CHECK_SPAN(qcc_expr_kind_name(QCC_EXPR_CAST),
                     strlen(qcc_expr_kind_name(QCC_EXPR_CAST)),
                     "cast", "cast name");
    QTEST_CHECK_SPAN(qcc_expr_kind_name(QCC_EXPR_SIZEOF_TYPE),
                     strlen(qcc_expr_kind_name(QCC_EXPR_SIZEOF_TYPE)),
                     "sizeof-type", "sizeof-type name");
    QTEST_CHECK_SPAN(qcc_expr_kind_name(QCC_EXPR_ALIGNOF_TYPE),
                     strlen(qcc_expr_kind_name(QCC_EXPR_ALIGNOF_TYPE)),
                     "alignof-type", "alignof-type name");
    QTEST_CHECK_SPAN(qcc_expr_kind_name((qcc_expr_kind)999),
                     strlen(qcc_expr_kind_name((qcc_expr_kind)999)),
                     "unknown", "out-of-range kind is total");
}

static void test_invalid_args(void)
{
    QTEST_CHECK_EQ_INT(qcc_ast_init(NULL), QCC_ERR_INVALID_ARGUMENT, "null init");
    qcc_ast_dispose(NULL); /* NULL-safe. */

    qcc_ast ast;
    qcc_ast_init(&ast);
    char  *dump = NULL;
    size_t len  = 0;
    QTEST_CHECK_EQ_INT(qcc_expr_dump(NULL, &dump, &len),
                       QCC_ERR_INVALID_ARGUMENT, "null expr dump");
    QTEST_CHECK_TRUE(qcc_expr_unary(&ast, QCC_PUNCT_MINUS, NULL, NULL) == NULL);
    qcc_ast_dispose(&ast);
    QTEST_CHECK_TRUE(1);
}

int main(void)
{
    test_leaves();
    test_operators();
    test_type_operands();
    test_kind_names();
    test_invalid_args();
    return qtest_report("ast");
}
