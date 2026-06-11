/*
 * Tests for the lexer: translation phases 1-3 against hand-checked token
 * streams. Each case states the §6.4 rule it locks down. Trigraph-looking
 * text in THIS file is written "?\?X" so a conforming seed compiler in C11
 * mode does not translate it inside our string literals.
 */
#include "qtest.h"

#include "diag/diag.h"
#include "lexer/lexer.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

enum { MAX_TOKENS = 64 };

/*
 * Lex `text` completely. Returns the token count INCLUDING the final EOF
 * token. The caller owns src/diags disposal (they are init'ed here).
 */
static size_t lex_all(const char *text, qcc_pp_token *toks, size_t max,
                      qcc_source *src, qcc_diag_sink *diags)
{
    qcc_diag_sink_init(diags);
    qcc_status st = qcc_source_from_memory("t.c", text, strlen(text), src);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "source init");

    qcc_lexer lx;
    qcc_lexer_init(&lx, src, diags);

    size_t n = 0;
    for (;;) {
        qcc_pp_token t;
        st = qcc_lexer_next(&lx, &t);
        QTEST_CHECK_EQ_INT(st, QCC_OK, "lexer_next status");
        if (n < max) {
            toks[n] = t;
        }
        n += 1;
        if (t.kind == QCC_PP_TOKEN_EOF || n >= max) {
            break;
        }
    }
    return n;
}

/* Assert one token's kind and splice-free spelling. */
static void check_tok(const qcc_source *src, const qcc_pp_token *tok,
                      qcc_pp_token_kind kind, const char *spelling)
{
    QTEST_CHECK_EQ_INT(tok->kind, kind, "token kind");
    char   buf[128];
    size_t n = qcc_lexer_token_spelling(src, tok, buf, sizeof(buf));
    QTEST_CHECK_SPAN(buf, n, spelling, "token spelling");
}

/* A plain function-definition skeleton: kinds, spellings, keyword lookup,
   line/column caching, and the newline tokens between logical lines. */
static void test_basic_stream(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];
    size_t n = lex_all("int main(void)\n{\n  return 0;\n}\n", t, MAX_TOKENS,
                       &src, &diags);

    QTEST_CHECK_EQ_UINT(n, 15, "token count");
    check_tok(&src, &t[0], QCC_PP_TOKEN_IDENTIFIER, "int");
    check_tok(&src, &t[1], QCC_PP_TOKEN_IDENTIFIER, "main");
    check_tok(&src, &t[2], QCC_PP_TOKEN_PUNCT, "(");
    check_tok(&src, &t[3], QCC_PP_TOKEN_IDENTIFIER, "void");
    check_tok(&src, &t[4], QCC_PP_TOKEN_PUNCT, ")");
    QTEST_CHECK_EQ_INT(t[5].kind, QCC_PP_TOKEN_NEWLINE, "newline 1");
    check_tok(&src, &t[6], QCC_PP_TOKEN_PUNCT, "{");
    QTEST_CHECK_EQ_INT(t[7].kind, QCC_PP_TOKEN_NEWLINE, "newline 2");
    check_tok(&src, &t[8], QCC_PP_TOKEN_IDENTIFIER, "return");
    check_tok(&src, &t[9], QCC_PP_TOKEN_PP_NUMBER, "0");
    check_tok(&src, &t[10], QCC_PP_TOKEN_PUNCT, ";");
    QTEST_CHECK_EQ_INT(t[11].kind, QCC_PP_TOKEN_NEWLINE, "newline 3");
    check_tok(&src, &t[12], QCC_PP_TOKEN_PUNCT, "}");
    QTEST_CHECK_EQ_INT(t[14].kind, QCC_PP_TOKEN_EOF, "eof");

    /* Keywords are identifiers at this stage (§6.4.1 binds at phase 7) —
       the keyword table resolves them on demand. */
    char   buf[16];
    size_t len = qcc_lexer_token_spelling(&src, &t[0], buf, sizeof(buf));
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup(buf, len), QCC_KW_INT, "int kw");
    len = qcc_lexer_token_spelling(&src, &t[1], buf, sizeof(buf));
    QTEST_CHECK_EQ_INT(qcc_keyword_lookup(buf, len), QCC_KW_NONE, "main not kw");

    /* Cached locations: "main" is line 1 column 5; "return" line 3 col 3. */
    QTEST_CHECK_EQ_UINT(t[1].line, 1, "main line");
    QTEST_CHECK_EQ_UINT(t[1].column, 5, "main column");
    QTEST_CHECK_EQ_UINT(t[8].line, 3, "return line");
    QTEST_CHECK_EQ_UINT(t[8].column, 3, "return column");

    QTEST_CHECK_EQ_UINT(qcc_diag_count(&diags), 0, "no diagnostics");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Maximal munch (§6.4 ¶4), including the classic a+++++b decomposition and
   the digraphs of §6.4.6 ¶3 folding to primary punctuators. */
static void test_maximal_munch(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    /* "a+++++b" lexes ++ ++ + (then b): munch never backtracks, even though
       a++ + ++b would parse and this will not. */
    size_t n = lex_all("a+++++b\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(n, 7, "a+++++b count");
    QTEST_CHECK_EQ_INT(t[1].punct, QCC_PUNCT_PLUS_PLUS, "++ (1)");
    QTEST_CHECK_EQ_INT(t[2].punct, QCC_PUNCT_PLUS_PLUS, "++ (2)");
    QTEST_CHECK_EQ_INT(t[3].punct, QCC_PUNCT_PLUS, "+");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* Three-char operators, ellipsis vs two dots, and digraphs. */
    n = lex_all("x >>= 1; ... .. %:%: <: :> <% %> %: ## #\n", t, MAX_TOKENS,
                &src, &diags);
    QTEST_CHECK_EQ_INT(t[1].punct, QCC_PUNCT_RSHIFT_EQ, ">>=");
    QTEST_CHECK_EQ_INT(t[4].punct, QCC_PUNCT_ELLIPSIS, "...");
    QTEST_CHECK_EQ_INT(t[5].punct, QCC_PUNCT_DOT, ".. is dot+dot (1)");
    QTEST_CHECK_EQ_INT(t[6].punct, QCC_PUNCT_DOT, ".. is dot+dot (2)");
    QTEST_CHECK_EQ_INT(t[7].punct, QCC_PUNCT_HASH_HASH, "%%:%%: is ##");
    QTEST_CHECK_EQ_INT(t[8].punct, QCC_PUNCT_LBRACKET, "<: is [");
    QTEST_CHECK_EQ_INT(t[9].punct, QCC_PUNCT_RBRACKET, ":> is ]");
    QTEST_CHECK_EQ_INT(t[10].punct, QCC_PUNCT_LBRACE, "<%% is {");
    QTEST_CHECK_EQ_INT(t[11].punct, QCC_PUNCT_RBRACE, "%%> is }");
    QTEST_CHECK_EQ_INT(t[12].punct, QCC_PUNCT_HASH, "%%: is #");
    QTEST_CHECK_EQ_INT(t[13].punct, QCC_PUNCT_HASH_HASH, "##");
    QTEST_CHECK_EQ_INT(t[14].punct, QCC_PUNCT_HASH, "#");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* pp-number extents (§6.4.8): hex floats, leading dot, sign after e/E/p/P
   only — including the surprising one-token "0xE+1". */
static void test_pp_numbers(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];
    size_t n = lex_all("0x1p-3 .5e+2 0xE+1 3..14 1e+ x+1\n", t, MAX_TOKENS,
                       &src, &diags);

    check_tok(&src, &t[0], QCC_PP_TOKEN_PP_NUMBER, "0x1p-3");
    check_tok(&src, &t[1], QCC_PP_TOKEN_PP_NUMBER, ".5e+2");
    check_tok(&src, &t[2], QCC_PP_TOKEN_PP_NUMBER, "0xE+1");
    check_tok(&src, &t[3], QCC_PP_TOKEN_PP_NUMBER, "3..14");
    check_tok(&src, &t[4], QCC_PP_TOKEN_PP_NUMBER, "1e+");
    /* ...but after an identifier, '+' is an operator as usual. */
    check_tok(&src, &t[5], QCC_PP_TOKEN_IDENTIFIER, "x");
    QTEST_CHECK_EQ_INT(t[6].punct, QCC_PUNCT_PLUS, "x+1 plus");
    check_tok(&src, &t[7], QCC_PP_TOKEN_PP_NUMBER, "1");

    QTEST_CHECK_EQ_UINT(n, 10, "pp-number token count");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Phase 2: a splice may sit inside ANY token. The physical span keeps the
   backslash-newline bytes; the spelling drops them. */
static void test_line_splice(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    /* "in\<nl>t" is the single identifier "int" (then keyword at phase 7). */
    size_t n = lex_all("in\\\nt x = 1;\n", t, MAX_TOKENS, &src, &diags);
    check_tok(&src, &t[0], QCC_PP_TOKEN_IDENTIFIER, "int");
    QTEST_CHECK_EQ_UINT(t[0].length, 5, "physical span includes the splice");
    QTEST_CHECK_EQ_UINT(n, 7, "spliced stream count");
    /* No NEWLINE token came from the spliced line break. */
    QTEST_CHECK_EQ_INT(t[1].kind, QCC_PP_TOKEN_IDENTIFIER, "x follows");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* A splice between the two '<' of a shift operator. */
    n = lex_all("a <\\\n< b\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_INT(t[1].kind, QCC_PP_TOKEN_PUNCT, "spliced << kind");
    QTEST_CHECK_EQ_INT(t[1].punct, QCC_PUNCT_LSHIFT, "spliced << punct");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Phase 3: each comment becomes one space — so a block comment swallows its
   interior newlines (directives can span lines through one), and the token
   after a comment carries leading_space. */
static void test_comments(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    size_t n = lex_all("a/*x*/b\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(n, 4, "comment-between count");
    QTEST_CHECK_EQ_UINT(t[0].leading_space, 0, "a has no leading space");
    QTEST_CHECK_EQ_UINT(t[1].leading_space, 1, "comment = one space before b");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* The newline INSIDE the block comment produces no NEWLINE token. */
    n = lex_all("a /* line1\nline2 */ b\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(n, 4, "multiline comment count");
    check_tok(&src, &t[1], QCC_PP_TOKEN_IDENTIFIER, "b");
    QTEST_CHECK_EQ_UINT(t[1].at_line_start, 0, "b is not first on a line");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* Line comments run to the newline; the newline itself survives. */
    n = lex_all("// hello\nz\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(n, 4, "line comment count");
    QTEST_CHECK_EQ_INT(t[0].kind, QCC_PP_TOKEN_NEWLINE, "newline survives");
    check_tok(&src, &t[1], QCC_PP_TOKEN_IDENTIFIER, "z");
    QTEST_CHECK_EQ_UINT(t[1].at_line_start, 1, "z starts its line");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* Unterminated block comment: one error, stream still terminates. */
    n = lex_all("a /* never closed", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&diags, QCC_DIAG_ERROR), 1,
                        "unterminated comment diagnosed");
    QTEST_CHECK_EQ_INT(t[n - 1].kind, QCC_PP_TOKEN_EOF, "stream ends");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* String/char literals: prefixes are part of the token (maximal munch) but
   only the §6.4.4.4/§6.4.5 prefixes; escapes extend, not close, the body. */
static void test_literals(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    size_t n = lex_all("u8\"hi\" L'a' u8x\"y\" \"a\\\"b\" '\\''\n", t,
                       MAX_TOKENS, &src, &diags);
    check_tok(&src, &t[0], QCC_PP_TOKEN_STRING_LIT, "u8\"hi\"");
    check_tok(&src, &t[1], QCC_PP_TOKEN_CHAR_CONST, "L'a'");
    /* u8x is no prefix: identifier, then a separate string. */
    check_tok(&src, &t[2], QCC_PP_TOKEN_IDENTIFIER, "u8x");
    check_tok(&src, &t[3], QCC_PP_TOKEN_STRING_LIT, "\"y\"");
    /* \" does not close; \' does not close. */
    check_tok(&src, &t[4], QCC_PP_TOKEN_STRING_LIT, "\"a\\\"b\"");
    check_tok(&src, &t[5], QCC_PP_TOKEN_CHAR_CONST, "'\\''");
    QTEST_CHECK_EQ_UINT(n, 8, "literal stream count");
    QTEST_CHECK_EQ_UINT(qcc_diag_count(&diags), 0, "no diagnostics");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* A newline inside a string is ill-formed: diagnosed, token ends there. */
    n = lex_all("\"abc\nrest\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&diags, QCC_DIAG_ERROR), 1,
                        "unterminated string diagnosed");
    check_tok(&src, &t[0], QCC_PP_TOKEN_STRING_LIT, "\"abc");
    QTEST_CHECK_EQ_INT(t[1].kind, QCC_PP_TOKEN_NEWLINE, "newline follows");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* UCNs (§6.4.3) extend identifiers; a malformed one is diagnosed. */
static void test_ucn_identifiers(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    size_t n = lex_all("caf\\u00E9 = 1;\n", t, MAX_TOKENS, &src, &diags);
    check_tok(&src, &t[0], QCC_PP_TOKEN_IDENTIFIER, "caf\\u00E9");
    QTEST_CHECK_EQ_UINT(n, 6, "ucn stream count");
    QTEST_CHECK_EQ_UINT(qcc_diag_count(&diags), 0, "valid ucn: no diags");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* "\uXY" with too few hex digits: stray backslash error, "other" token. */
    n = lex_all("\\uZ\n", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&diags, QCC_DIAG_ERROR), 1,
                        "malformed ucn diagnosed");
    QTEST_CHECK_EQ_INT(t[0].kind, QCC_PP_TOKEN_OTHER, "stray backslash");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Directive shape facts the preprocessor will rely on (§6.10 ¶2). */
static void test_line_start_flags(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];
    size_t n = lex_all("#define X 1\n  #if X\n", t, MAX_TOKENS, &src, &diags);

    QTEST_CHECK_EQ_INT(t[0].punct, QCC_PUNCT_HASH, "hash");
    QTEST_CHECK_EQ_UINT(t[0].at_line_start, 1, "# first on line");
    QTEST_CHECK_EQ_UINT(t[1].at_line_start, 0, "define is not");
    /* Whitespace before # is fine — it is still the first pp-token. */
    QTEST_CHECK_EQ_INT(t[5].punct, QCC_PUNCT_HASH, "indented hash");
    QTEST_CHECK_EQ_UINT(t[5].at_line_start, 1, "indented # first on line");
    QTEST_CHECK_EQ_UINT(t[5].leading_space, 1, "indented # has space before");
    QTEST_CHECK_EQ_UINT(n, 10, "directive stream count");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Trigraphs: warned, not translated (ADR-0013); characters lex literally. */
static void test_trigraph_warning(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];
    size_t n = lex_all("a ?\?= b\n", t, MAX_TOKENS, &src, &diags);

    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&diags, QCC_DIAG_WARNING), 1,
                        "trigraph warned");
    QTEST_CHECK_EQ_INT(t[1].punct, QCC_PUNCT_QUESTION, "? stays ?");
    QTEST_CHECK_EQ_INT(t[2].punct, QCC_PUNCT_QUESTION, "second ?");
    QTEST_CHECK_EQ_INT(t[3].punct, QCC_PUNCT_EQ, "= stays =");
    QTEST_CHECK_EQ_UINT(n, 7, "trigraph stream count");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* "other" pp-tokens (§6.4 ¶1) and the missing-final-newline repair. */
static void test_other_and_eof(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_pp_token  t[MAX_TOKENS];

    size_t n = lex_all("@", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_INT(t[0].kind, QCC_PP_TOKEN_OTHER, "@ is other");
    /* No trailing newline in the input: one is synthesized, then EOF. */
    QTEST_CHECK_EQ_INT(t[1].kind, QCC_PP_TOKEN_NEWLINE, "synthesized newline");
    QTEST_CHECK_EQ_UINT(t[1].length, 0, "synthesized newline is empty");
    QTEST_CHECK_EQ_INT(t[2].kind, QCC_PP_TOKEN_EOF, "eof");
    QTEST_CHECK_EQ_UINT(n, 3, "other/eof count");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);

    /* Empty input: EOF directly, no synthetic newline. */
    n = lex_all("", t, MAX_TOKENS, &src, &diags);
    QTEST_CHECK_EQ_UINT(n, 1, "empty input is just eof");
    QTEST_CHECK_EQ_INT(t[0].kind, QCC_PP_TOKEN_EOF, "eof only");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

/* Header-name mode (§6.4.7): explicit, preprocessor-controlled. */
static void test_header_name_mode(void)
{
    qcc_source    src;
    qcc_diag_sink diags;
    qcc_diag_sink_init(&diags);

    qcc_status st = qcc_source_from_memory("t.c", "#include <stdio.h>\n",
                                           19, &src);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "source init");

    qcc_lexer lx;
    qcc_lexer_init(&lx, &src, &diags);

    qcc_pp_token t;
    QTEST_CHECK_EQ_INT(qcc_lexer_next(&lx, &t), QCC_OK, "lex #");
    QTEST_CHECK_EQ_INT(t.punct, QCC_PUNCT_HASH, "#");
    QTEST_CHECK_EQ_INT(qcc_lexer_next(&lx, &t), QCC_OK, "lex include");
    QTEST_CHECK_EQ_INT(t.kind, QCC_PP_TOKEN_IDENTIFIER, "include ident");

    /* This is the moment the (future) preprocessor flips the switch. */
    qcc_lexer_set_header_mode(&lx, 1);
    QTEST_CHECK_EQ_INT(qcc_lexer_next(&lx, &t), QCC_OK, "lex header name");
    QTEST_CHECK_EQ_INT(t.kind, QCC_PP_TOKEN_HEADER_NAME, "header-name kind");
    check_tok(&src, &t, QCC_PP_TOKEN_HEADER_NAME, "<stdio.h>");
    qcc_lexer_set_header_mode(&lx, 0);

    QTEST_CHECK_EQ_INT(qcc_lexer_next(&lx, &t), QCC_OK, "lex newline");
    QTEST_CHECK_EQ_INT(t.kind, QCC_PP_TOKEN_NEWLINE, "newline");

    QTEST_CHECK_EQ_UINT(qcc_diag_count(&diags), 0, "no diagnostics");
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&src);
}

int main(void)
{
    test_basic_stream();
    test_maximal_munch();
    test_pp_numbers();
    test_line_splice();
    test_comments();
    test_literals();
    test_ucn_identifiers();
    test_line_start_flags();
    test_trigraph_warning();
    test_other_and_eof();
    test_header_name_mode();
    return qtest_report("lexer");
}
