/*
 * qcc — preprocessing tokens: name tables and keyword lookup.
 *
 * Everything here is static const data plus total functions over it. The
 * keyword table is sorted by spelling and searched with binary search: 44
 * entries make the asymptotics irrelevant, but the sorted table also serves as
 * documentation (one glance shows the full §6.4.1 list) and the search code is
 * the same that a perfect-hash replacement would slot into later if profiling
 * ever cares.
 */
#include "token/token.h"

#include <string.h>

const char *qcc_pp_token_kind_name(qcc_pp_token_kind kind)
{
    switch (kind) {
    case QCC_PP_TOKEN_EOF:         return "eof";
    case QCC_PP_TOKEN_NEWLINE:     return "newline";
    case QCC_PP_TOKEN_IDENTIFIER:  return "identifier";
    case QCC_PP_TOKEN_PP_NUMBER:   return "pp-number";
    case QCC_PP_TOKEN_CHAR_CONST:  return "character-constant";
    case QCC_PP_TOKEN_STRING_LIT:  return "string-literal";
    case QCC_PP_TOKEN_PUNCT:       return "punctuator";
    case QCC_PP_TOKEN_HEADER_NAME: return "header-name";
    case QCC_PP_TOKEN_OTHER:       return "other";
    }
    return "unknown"; /* Out-of-range integer cast to the enum; stay total. */
}

const char *qcc_token_kind_name(qcc_token_kind kind)
{
    switch (kind) {
    case QCC_TOKEN_EOF:        return "eof";
    case QCC_TOKEN_KEYWORD:    return "keyword";
    case QCC_TOKEN_IDENTIFIER: return "identifier";
    case QCC_TOKEN_INTEGER:    return "integer-constant";
    case QCC_TOKEN_FLOATING:   return "floating-constant";
    case QCC_TOKEN_CHAR:       return "character-constant";
    case QCC_TOKEN_STRING:     return "string-literal";
    case QCC_TOKEN_PUNCT:      return "punctuator";
    }
    return "unknown"; /* Out-of-range integer cast to the enum; stay total. */
}

/*
 * Primary spellings, indexed by qcc_punct. The order MUST match the enum in
 * token.h exactly; test_token locks the correspondence down entry by entry so
 * a reorder in either place fails the suite instead of mislabeling tokens.
 */
static const char *const punct_spellings[QCC_PUNCT_COUNT] = {
    "[",  "]",  "(",  ")",  "{",  "}",  ".",  "->",
    "++", "--", "&",  "*",  "+",  "-",  "~",  "!",
    "/",  "%",  "<<", ">>", "<",  ">",  "<=", ">=",
    "==", "!=", "^",  "|",  "&&", "||",
    "?",  ":",  ";",  "...",
    "=",  "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|=",
    ",",  "#",  "##"
};

const char *qcc_punct_str(qcc_punct punct)
{
    if ((int)punct < 0 || punct >= QCC_PUNCT_COUNT) {
        return "?";
    }
    return punct_spellings[punct];
}

/* One keyword-table entry. `length` is precomputed so lookup never strlen()s. */
typedef struct keyword_entry {
    const char *spelling;
    size_t      length;
    qcc_keyword keyword;
} keyword_entry;

#define KW(text, value) { text, sizeof(text) - 1, value }

/*
 * The 44 keywords of C11 (§6.4.1 ¶1), sorted by spelling in the byte order
 * memcmp uses (so '_' < lowercase letters: ASCII 0x5F < 0x61). Binary search
 * below depends on this order; test_token verifies it mechanically.
 */
static const keyword_entry keyword_table[] = {
    KW("_Alignas",       QCC_KW_ALIGNAS),
    KW("_Alignof",       QCC_KW_ALIGNOF),
    KW("_Atomic",        QCC_KW_ATOMIC),
    KW("_Bool",          QCC_KW_BOOL),
    KW("_Complex",       QCC_KW_COMPLEX),
    KW("_Generic",       QCC_KW_GENERIC),
    KW("_Imaginary",     QCC_KW_IMAGINARY),
    KW("_Noreturn",      QCC_KW_NORETURN),
    KW("_Static_assert", QCC_KW_STATIC_ASSERT),
    KW("_Thread_local",  QCC_KW_THREAD_LOCAL),
    KW("auto",           QCC_KW_AUTO),
    KW("break",          QCC_KW_BREAK),
    KW("case",           QCC_KW_CASE),
    KW("char",           QCC_KW_CHAR),
    KW("const",          QCC_KW_CONST),
    KW("continue",       QCC_KW_CONTINUE),
    KW("default",        QCC_KW_DEFAULT),
    KW("do",             QCC_KW_DO),
    KW("double",         QCC_KW_DOUBLE),
    KW("else",           QCC_KW_ELSE),
    KW("enum",           QCC_KW_ENUM),
    KW("extern",         QCC_KW_EXTERN),
    KW("float",          QCC_KW_FLOAT),
    KW("for",            QCC_KW_FOR),
    KW("goto",           QCC_KW_GOTO),
    KW("if",             QCC_KW_IF),
    KW("inline",         QCC_KW_INLINE),
    KW("int",            QCC_KW_INT),
    KW("long",           QCC_KW_LONG),
    KW("register",       QCC_KW_REGISTER),
    KW("restrict",       QCC_KW_RESTRICT),
    KW("return",         QCC_KW_RETURN),
    KW("short",          QCC_KW_SHORT),
    KW("signed",         QCC_KW_SIGNED),
    KW("sizeof",         QCC_KW_SIZEOF),
    KW("static",         QCC_KW_STATIC),
    KW("struct",         QCC_KW_STRUCT),
    KW("switch",         QCC_KW_SWITCH),
    KW("typedef",        QCC_KW_TYPEDEF),
    KW("union",          QCC_KW_UNION),
    KW("unsigned",       QCC_KW_UNSIGNED),
    KW("void",           QCC_KW_VOID),
    KW("volatile",       QCC_KW_VOLATILE),
    KW("while",          QCC_KW_WHILE)
};

#undef KW

enum { KEYWORD_TABLE_LEN = sizeof(keyword_table) / sizeof(keyword_table[0]) };

/*
 * memcmp-order comparison of a length-delimited span against a table entry:
 * compare the common prefix, then break ties by length (shorter sorts first,
 * matching how a sorted list of strings orders "do" before "double").
 */
static int span_cmp(const char *bytes, size_t length, const keyword_entry *entry)
{
    size_t common = (length < entry->length) ? length : entry->length;
    int    diff   = memcmp(bytes, entry->spelling, common);
    if (diff != 0) {
        return diff;
    }
    if (length == entry->length) {
        return 0;
    }
    return (length < entry->length) ? -1 : 1;
}

qcc_keyword qcc_keyword_lookup(const char *bytes, size_t length)
{
    if (bytes == NULL || length == 0) {
        return QCC_KW_NONE;
    }

    size_t lo = 0;
    size_t hi = KEYWORD_TABLE_LEN;
    while (lo < hi) {
        size_t mid  = lo + (hi - lo) / 2;
        int    diff = span_cmp(bytes, length, &keyword_table[mid]);
        if (diff == 0) {
            return keyword_table[mid].keyword;
        }
        if (diff < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return QCC_KW_NONE;
}

const char *qcc_keyword_str(qcc_keyword keyword)
{
    /*
     * The table is sorted by spelling, not enum value, so render via a linear
     * scan: this function serves diagnostics and tests, never hot paths.
     */
    for (size_t i = 0; i < KEYWORD_TABLE_LEN; ++i) {
        if (keyword_table[i].keyword == keyword) {
            return keyword_table[i].spelling;
        }
    }
    return "?"; /* QCC_KW_NONE, QCC_KW_COUNT, or out of range. */
}
