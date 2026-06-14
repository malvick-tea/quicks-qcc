/*
 * Tests for the token module: punctuator-spelling correspondence, keyword
 * lookup over spans (hits, misses, prefix traps), and stable kind names.
 */
#include "qtest.h"

#include "token/token.h"

/*
 * Lock the enum-order <-> spelling-table correspondence down completely: every
 * punctuator is checked by (enumerator, spelling) pair, so reordering either
 * side of the table in token.c fails here instead of silently mislabeling
 * tokens. The pairs themselves restate §6.4.6 ¶1.
 */
static void test_punct_spellings(void)
{
    static const struct { qcc_punct punct; const char *spelling; } cases[] = {
        { QCC_PUNCT_LBRACKET, "[" },   { QCC_PUNCT_RBRACKET, "]" },
        { QCC_PUNCT_LPAREN, "(" },     { QCC_PUNCT_RPAREN, ")" },
        { QCC_PUNCT_LBRACE, "{" },     { QCC_PUNCT_RBRACE, "}" },
        { QCC_PUNCT_DOT, "." },        { QCC_PUNCT_ARROW, "->" },
        { QCC_PUNCT_PLUS_PLUS, "++" }, { QCC_PUNCT_MINUS_MINUS, "--" },
        { QCC_PUNCT_AMP, "&" },        { QCC_PUNCT_STAR, "*" },
        { QCC_PUNCT_PLUS, "+" },       { QCC_PUNCT_MINUS, "-" },
        { QCC_PUNCT_TILDE, "~" },      { QCC_PUNCT_BANG, "!" },
        { QCC_PUNCT_SLASH, "/" },      { QCC_PUNCT_PERCENT, "%" },
        { QCC_PUNCT_LSHIFT, "<<" },    { QCC_PUNCT_RSHIFT, ">>" },
        { QCC_PUNCT_LT, "<" },         { QCC_PUNCT_GT, ">" },
        { QCC_PUNCT_LE, "<=" },        { QCC_PUNCT_GE, ">=" },
        { QCC_PUNCT_EQ_EQ, "==" },     { QCC_PUNCT_BANG_EQ, "!=" },
        { QCC_PUNCT_CARET, "^" },      { QCC_PUNCT_PIPE, "|" },
        { QCC_PUNCT_AMP_AMP, "&&" },   { QCC_PUNCT_PIPE_PIPE, "||" },
        { QCC_PUNCT_QUESTION, "?" },   { QCC_PUNCT_COLON, ":" },
        { QCC_PUNCT_SEMI, ";" },       { QCC_PUNCT_ELLIPSIS, "..." },
        { QCC_PUNCT_EQ, "=" },         { QCC_PUNCT_STAR_EQ, "*=" },
        { QCC_PUNCT_SLASH_EQ, "/=" },  { QCC_PUNCT_PERCENT_EQ, "%=" },
        { QCC_PUNCT_PLUS_EQ, "+=" },   { QCC_PUNCT_MINUS_EQ, "-=" },
        { QCC_PUNCT_LSHIFT_EQ, "<<=" },{ QCC_PUNCT_RSHIFT_EQ, ">>=" },
        { QCC_PUNCT_AMP_EQ, "&=" },    { QCC_PUNCT_CARET_EQ, "^=" },
        { QCC_PUNCT_PIPE_EQ, "|=" },   { QCC_PUNCT_COMMA, "," },
        { QCC_PUNCT_HASH, "#" },       { QCC_PUNCT_HASH_HASH, "##" }
    };
    enum { CASE_COUNT = sizeof(cases) / sizeof(cases[0]) };

    /* The pair list itself must be exhaustive over the enum. */
    QTEST_CHECK_EQ_INT(CASE_COUNT, QCC_PUNCT_COUNT, "all punctuators covered");

    for (size_t i = 0; i < CASE_COUNT; ++i) {
        const char *got = qcc_punct_str(cases[i].punct);
        QTEST_CHECK_SPAN(got, strlen(got), cases[i].spelling, "punct spelling");
    }

    QTEST_CHECK_SPAN(qcc_punct_str((qcc_punct)-1),
                     strlen(qcc_punct_str((qcc_punct)-1)), "?",
                     "out-of-range punct is total");
}

/* Every §6.4.1 keyword must round-trip: spelling -> enum -> spelling. */
static void test_keyword_hits(void)
{
    int found = 0;
    for (int kw = QCC_KW_NONE + 1; kw < QCC_KW_COUNT; ++kw) {
        const char *spelling = qcc_keyword_str((qcc_keyword)kw);
        QTEST_CHECK_TRUE(spelling != NULL && spelling[0] != '?');

        qcc_keyword back = qcc_keyword_lookup(spelling, strlen(spelling));
        QTEST_CHECK_EQ_INT(back, kw, "keyword round-trip");
        ++found;
    }
    /* 44 keywords in C11 §6.4.1 ¶1 — neither more nor fewer. */
    QTEST_CHECK_EQ_INT(found, 44, "C11 keyword count");
}

static void test_keyword_misses(void)
{
    /* Not keywords at all. */
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("main", 4), QCC_KW_NONE, "main");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("Bool", 4), QCC_KW_NONE, "Bool");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("", 0), QCC_KW_NONE, "empty");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup(NULL, 0), QCC_KW_NONE, "NULL");

    /* Length must participate exactly: prefixes and extensions are misses.
       "do"/"double" share a prefix — the classic sorted-table tie-break trap. */
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("do", 2), QCC_KW_DO, "do hits");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("dou", 3), QCC_KW_NONE, "dou misses");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("double", 6), QCC_KW_DOUBLE, "double");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("doublee", 7), QCC_KW_NONE, "doublee");
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("whil", 4), QCC_KW_NONE, "whil");

    /* A span is not NUL-terminated: only the first `length` bytes count. */
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup("integer", 3), QCC_KW_INT,
                       "span-delimited lookup");
}

static void test_kind_names(void)
{
    QTEST_CHECK_SPAN(qcc_pp_token_kind_name(QCC_PP_TOKEN_PP_NUMBER),
                     strlen(qcc_pp_token_kind_name(QCC_PP_TOKEN_PP_NUMBER)),
                     "pp-number", "pp-number name");
    QTEST_CHECK_SPAN(qcc_pp_token_kind_name(QCC_PP_TOKEN_EOF),
                     strlen(qcc_pp_token_kind_name(QCC_PP_TOKEN_EOF)),
                     "eof", "eof name");
    QTEST_CHECK_SPAN(qcc_pp_token_kind_name((qcc_pp_token_kind)999),
                     strlen(qcc_pp_token_kind_name((qcc_pp_token_kind)999)),
                     "unknown", "out-of-range kind is total");
}

/* The constant-value naming helpers used by diagnostics and the token dump. */
static void test_value_names(void)
{
    QTEST_CHECK_SPAN(qcc_int_type_name(QCC_INT_ULLONG),
                     strlen(qcc_int_type_name(QCC_INT_ULLONG)),
                     "unsigned long long", "int type name");
    QTEST_CHECK_SPAN(qcc_float_type_name(QCC_FLOAT_LDOUBLE),
                     strlen(qcc_float_type_name(QCC_FLOAT_LDOUBLE)),
                     "long double", "float type name");

    static const struct {
        qcc_char_encoding enc;
        const char       *name;
    } encs[] = {
        { QCC_ENC_PLAIN,  "plain" },  { QCC_ENC_WIDE,   "wide" },
        { QCC_ENC_CHAR16, "char16" }, { QCC_ENC_CHAR32, "char32" },
        { QCC_ENC_UTF8,   "utf8" },
    };
    for (size_t i = 0; i < sizeof(encs) / sizeof(encs[0]); ++i) {
        QTEST_CHECK_SPAN(qcc_char_encoding_str(encs[i].enc),
                         strlen(qcc_char_encoding_str(encs[i].enc)),
                         encs[i].name, "encoding name");
    }
    QTEST_CHECK_SPAN(qcc_char_encoding_str((qcc_char_encoding)999),
                     strlen(qcc_char_encoding_str((qcc_char_encoding)999)),
                     "unknown", "out-of-range encoding is total");
}

int main(void)
{
    test_punct_spellings();
    test_keyword_hits();
    test_keyword_misses();
    test_kind_names();
    test_value_names();
    return qtest_report("token");
}
