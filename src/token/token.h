/*
 * qcc — preprocessing tokens (ISO C11 §6.4)
 *
 * Responsibility
 * Define the vocabulary that flows between the front-end stages: the
 * *preprocessing-token* (what the lexer produces and the preprocessor
 * transforms, §6.4 ¶1), the full C11 punctuator set (§6.4.6), and the C11
 * keyword table (§6.4.1) used when pp-tokens are later converted to tokens
 * (translation phase 7, §5.1.1.2). Pure data + naming functions; no I/O, no
 * allocation.
 *
 * Why pp-tokens and not "tokens" directly?
 *   The standard defines translation as eight phases; phases 3-6 operate on
 *   preprocessing-tokens, and only phase 7 converts each pp-token into a token
 *   (keywords resolved, constants evaluated). Modeling that distinction in the
 *   type system from day one (ADR-0013) means the preprocessor — which must see
 *   `#`, pp-numbers, and unexpanded identifiers — needs no retrofit later.
 *   Concretely: `0x1p-3` is ONE pp-number, and `else` is just an identifier to
 *   the preprocessor (it can even be #define'd); both facts are invisible in a
 *   "phase-7 tokens only" design.
 *
 * Spans, not copies
 *   A pp-token does not own text. It carries (offset, length) into the
 *   qcc_source it was lexed from; later stages read the spelling through the
 *   source. This keeps tokens trivially copyable values and diagnostics exact.
 */
#ifndef QCC_TOKEN_TOKEN_H
#define QCC_TOKEN_TOKEN_H

#include <stddef.h>
#include <stdint.h>

/*
 * The preprocessing-token categories of §6.4 ¶1, plus the two structural kinds
 * a line-oriented preprocessor needs (newline, end-of-input).
 *
 *   header-name  : produced only inside a #include directive (§6.4.7); the
 *                  lexer emits it only when explicitly switched into
 *                  header-name mode by the preprocessor, because `<stdio.h>`
 *                  is otherwise four punctuators and two identifiers.
 *   pp-number    : the deliberately over-broad numeric shape of §6.4.8 that
 *                  covers every integer AND floating constant (conversion and
 *                  range checking happen at phase 7, §6.4.4).
 *   other        : "each non-white-space character that cannot be one of the
 *                  above" (§6.4 ¶1) — e.g. `@`, backtick, a stray `\`. Kept as
 *                  a token (not an immediate error) because the standard only
 *                  forbids it if it survives to phase 7.
 */
typedef enum qcc_pp_token_kind {
    QCC_PP_TOKEN_EOF = 0,        /* End of input; always the final token.        */
    QCC_PP_TOKEN_NEWLINE,        /* End of a logical line (directives end here,
                                    §6.10 ¶2). Spliced lines emit no newline.    */
    QCC_PP_TOKEN_IDENTIFIER,     /* §6.4.2.1 (also keywords, at this stage).     */
    QCC_PP_TOKEN_PP_NUMBER,      /* §6.4.8.                                      */
    QCC_PP_TOKEN_CHAR_CONST,     /* §6.4.4.4, including L/u/U prefixes.          */
    QCC_PP_TOKEN_STRING_LIT,     /* §6.4.5, including u8/u/U/L prefixes.         */
    QCC_PP_TOKEN_PUNCT,          /* §6.4.6; which one is in the `punct` field.   */
    QCC_PP_TOKEN_HEADER_NAME,    /* §6.4.7, only in header-name mode.            */
    QCC_PP_TOKEN_OTHER           /* §6.4 ¶1 catch-all, see above.                */
} qcc_pp_token_kind;

/*
 * Every C11 punctuator (§6.4.6 ¶1), one enumerator per *token*, not per
 * spelling: the digraphs <: :> <% %> %: %:%: are alternate spellings that
 * "behave, respectively, the same" as [ ] { } # ## (§6.4.6 ¶3), so the lexer
 * maps them to these enumerators and the original spelling stays recoverable
 * from the token's source span.
 */
typedef enum qcc_punct {
    QCC_PUNCT_LBRACKET = 0,  /* [   (also spelled <:) */
    QCC_PUNCT_RBRACKET,      /* ]   (also spelled :>) */
    QCC_PUNCT_LPAREN,        /* (                     */
    QCC_PUNCT_RPAREN,        /* )                     */
    QCC_PUNCT_LBRACE,        /* {   (also spelled <%) */
    QCC_PUNCT_RBRACE,        /* }   (also spelled %>) */
    QCC_PUNCT_DOT,           /* .                     */
    QCC_PUNCT_ARROW,         /* ->                    */
    QCC_PUNCT_PLUS_PLUS,     /* ++                    */
    QCC_PUNCT_MINUS_MINUS,   /* --                    */
    QCC_PUNCT_AMP,           /* &                     */
    QCC_PUNCT_STAR,          /* *                     */
    QCC_PUNCT_PLUS,          /* +                     */
    QCC_PUNCT_MINUS,         /* -                     */
    QCC_PUNCT_TILDE,         /* ~                     */
    QCC_PUNCT_BANG,          /* !                     */
    QCC_PUNCT_SLASH,         /* /                     */
    QCC_PUNCT_PERCENT,       /* %                     */
    QCC_PUNCT_LSHIFT,        /* <<                    */
    QCC_PUNCT_RSHIFT,        /* >>                    */
    QCC_PUNCT_LT,            /* <                     */
    QCC_PUNCT_GT,            /* >                     */
    QCC_PUNCT_LE,            /* <=                    */
    QCC_PUNCT_GE,            /* >=                    */
    QCC_PUNCT_EQ_EQ,         /* ==                    */
    QCC_PUNCT_BANG_EQ,       /* !=                    */
    QCC_PUNCT_CARET,         /* ^                     */
    QCC_PUNCT_PIPE,          /* |                     */
    QCC_PUNCT_AMP_AMP,       /* &&                    */
    QCC_PUNCT_PIPE_PIPE,     /* ||                    */
    QCC_PUNCT_QUESTION,      /* ?                     */
    QCC_PUNCT_COLON,         /* :                     */
    QCC_PUNCT_SEMI,          /* ;                     */
    QCC_PUNCT_ELLIPSIS,      /* ...                   */
    QCC_PUNCT_EQ,            /* =                     */
    QCC_PUNCT_STAR_EQ,       /* *=                    */
    QCC_PUNCT_SLASH_EQ,      /* /=                    */
    QCC_PUNCT_PERCENT_EQ,    /* %=                    */
    QCC_PUNCT_PLUS_EQ,       /* +=                    */
    QCC_PUNCT_MINUS_EQ,      /* -=                    */
    QCC_PUNCT_LSHIFT_EQ,     /* <<=                   */
    QCC_PUNCT_RSHIFT_EQ,     /* >>=                   */
    QCC_PUNCT_AMP_EQ,        /* &=                    */
    QCC_PUNCT_CARET_EQ,      /* ^=                    */
    QCC_PUNCT_PIPE_EQ,       /* |=                    */
    QCC_PUNCT_COMMA,         /* ,                     */
    QCC_PUNCT_HASH,          /* #   (also spelled %:) */
    QCC_PUNCT_HASH_HASH,     /* ##  (also spelled %:%:) */

    QCC_PUNCT_COUNT          /* Sentinel = number of punctuators. */
} qcc_punct;

/*
 * The 44 keywords of C11 (§6.4.1 ¶1). At the pp-token level these are plain
 * identifiers; phase 7 turns an identifier into a keyword exactly when its
 * spelling matches this list (§6.4.1 ¶2: keywords cannot be used otherwise).
 * QCC_KW_NONE is the lookup miss value, deliberately 0 so `if (kw)` reads as
 * "is a keyword".
 */
typedef enum qcc_keyword {
    QCC_KW_NONE = 0,
    QCC_KW_AUTO, QCC_KW_BREAK, QCC_KW_CASE, QCC_KW_CHAR, QCC_KW_CONST,
    QCC_KW_CONTINUE, QCC_KW_DEFAULT, QCC_KW_DO, QCC_KW_DOUBLE, QCC_KW_ELSE,
    QCC_KW_ENUM, QCC_KW_EXTERN, QCC_KW_FLOAT, QCC_KW_FOR, QCC_KW_GOTO,
    QCC_KW_IF, QCC_KW_INLINE, QCC_KW_INT, QCC_KW_LONG, QCC_KW_REGISTER,
    QCC_KW_RESTRICT, QCC_KW_RETURN, QCC_KW_SHORT, QCC_KW_SIGNED,
    QCC_KW_SIZEOF, QCC_KW_STATIC, QCC_KW_STRUCT, QCC_KW_SWITCH,
    QCC_KW_TYPEDEF, QCC_KW_UNION, QCC_KW_UNSIGNED, QCC_KW_VOID,
    QCC_KW_VOLATILE, QCC_KW_WHILE,
    QCC_KW_ALIGNAS,        /* _Alignas       */
    QCC_KW_ALIGNOF,        /* _Alignof       */
    QCC_KW_ATOMIC,         /* _Atomic        */
    QCC_KW_BOOL,           /* _Bool          */
    QCC_KW_COMPLEX,        /* _Complex       */
    QCC_KW_GENERIC,        /* _Generic       */
    QCC_KW_IMAGINARY,      /* _Imaginary     */
    QCC_KW_NORETURN,       /* _Noreturn      */
    QCC_KW_STATIC_ASSERT,  /* _Static_assert */
    QCC_KW_THREAD_LOCAL,   /* _Thread_local  */

    QCC_KW_COUNT           /* Sentinel = QCC_KW_NONE + 44 keywords + 1. */
} qcc_keyword;

/*
 * One preprocessing token. A plain value: copy it freely, never free it. The
 * text lives in the qcc_source the token was lexed from, at [offset, offset
 * + length); a span may contain interior backslash-newline splices (§5.1.1.2
 * phase 2), so its *meaning* can differ from its raw bytes — later stages must
 * read spellings through a splice-aware accessor, not memcpy.
 */
typedef struct qcc_pp_token {
    qcc_pp_token_kind kind;
    qcc_punct         punct;          /* Valid iff kind == QCC_PP_TOKEN_PUNCT.   */

    size_t            offset;         /* Byte offset of the first character.     */
    size_t            length;         /* Physical span length (incl. splices).   */
    uint32_t          line;           /* 1-based line of `offset` (cached).      */
    uint32_t          column;         /* 1-based column of `offset` (cached).    */

    /*
     * Whitespace context, recorded because preprocessing is whitespace-
     * sensitive in exactly two ways: a directive's `#` must be the first
     * pp-token on a line (§6.10 ¶2), and stringization preserves inter-token
     * spacing (§6.10.3.2 ¶2). Comments count as whitespace (phase 3 replaces
     * each comment by one space).
     */
    unsigned          leading_space : 1;  /* Whitespace/comment just before it.  */
    unsigned          at_line_start : 1;  /* First pp-token on its logical line. */
} qcc_pp_token;

/* Stable lowercase name of a pp-token kind ("identifier", "pp-number", …). */
const char *qcc_pp_token_kind_name(qcc_pp_token_kind kind);

/* Forward declaration: a phase-7 token records its provenance source for
   diagnostics, but `token` stays free of a source.h dependency (ADR-0008). */
struct qcc_source;

/*
 * A phase-7 token (§6.4 ¶3) — the vocabulary the parser consumes, produced from
 * preprocessing tokens by the `convert` stage (translation phases 5-7,
 * §5.1.1.2). It differs from a preprocessing token in exactly the ways §6.4 ¶3
 * does: an identifier that spells a keyword has become a keyword (§6.4.1 ¶2), and
 * a pp-number (the over-broad §6.4.8 shape) has been classified as an integer or
 * floating constant (§6.4.4). A constant token carries both its source lexeme in
 * `spelling` and its evaluated *value* (the integer/floating/character value, or
 * the decoded string code units) in the value fields below, computed by the
 * `convert` units (ADR-0017).
 *
 * Like a preprocessing token it is a plain value (copy freely). `spelling` is
 * interned by the producing `convert` and valid for that stage's lifetime;
 * `source`/`offset`/`line`/`column` locate the token for diagnostics.
 */
typedef enum qcc_token_kind {
    QCC_TOKEN_EOF = 0,      /* End of the token stream; always the final token.   */
    QCC_TOKEN_KEYWORD,      /* §6.4.1; `keyword` is the resolved keyword.         */
    QCC_TOKEN_IDENTIFIER,   /* §6.4.2.1, and not a keyword.                       */
    QCC_TOKEN_INTEGER,      /* §6.4.4.1 integer constant.                         */
    QCC_TOKEN_FLOATING,     /* §6.4.4.2 floating constant.                        */
    QCC_TOKEN_CHAR,         /* §6.4.4.4 character constant.                       */
    QCC_TOKEN_STRING,       /* §6.4.5 string literal.                             */
    QCC_TOKEN_PUNCT         /* §6.4.6; `punct` says which.                        */
} qcc_token_kind;

/*
 * The type of an integer constant (§6.4.4.1 ¶5), as resolved against the
 * target's widths (x86-64 System V LP64: int 32, long 64, long long 64). The
 * order is the candidate-list order — a constant takes the first of its allowed
 * types whose representation holds its value.
 */
typedef enum qcc_int_type {
    QCC_INT_INT = 0,   /* int                */
    QCC_INT_UINT,      /* unsigned int       */
    QCC_INT_LONG,      /* long               */
    QCC_INT_ULONG,     /* unsigned long      */
    QCC_INT_LLONG,     /* long long          */
    QCC_INT_ULLONG     /* unsigned long long */
} qcc_int_type;

/* The type of a floating constant (§6.4.4.2 ¶4), set by its suffix: a plain
   constant is double, an `f`/`F` suffix makes it float, an `l`/`L` long double. */
typedef enum qcc_float_type {
    QCC_FLOAT_DOUBLE = 0,  /* double      (no suffix) */
    QCC_FLOAT_FLOAT,       /* float       (f / F)     */
    QCC_FLOAT_LDOUBLE      /* long double (l / L)     */
} qcc_float_type;

/* The encoding of a character constant or string literal, set by its prefix
   (§6.4.4.4 ¶2, §6.4.5 ¶3): none, L (wchar_t), u (char16_t), U (char32_t), and
   u8 (UTF-8, strings only). */
typedef enum qcc_char_encoding {
    QCC_ENC_PLAIN = 0,  /* (none)            */
    QCC_ENC_WIDE,       /* L  -> wchar_t     */
    QCC_ENC_CHAR16,     /* u  -> char16_t    */
    QCC_ENC_CHAR32,     /* U  -> char32_t    */
    QCC_ENC_UTF8        /* u8 -> char (UTF-8); string literals only */
} qcc_char_encoding;

typedef struct qcc_token {
    qcc_token_kind           kind;
    qcc_keyword              keyword;      /* Valid iff kind == QCC_TOKEN_KEYWORD. */
    qcc_punct                punct;        /* Valid iff kind == QCC_TOKEN_PUNCT.   */
    const char              *spelling;     /* Interned source lexeme; never NULL.  */
    size_t                   spelling_len;
    const struct qcc_source *source;       /* Provenance; may be NULL (EOF).       */
    size_t                   offset;
    uint32_t                 line;
    uint32_t                 column;
    unsigned                 leading_space : 1; /* Whitespace preceded it.         */

    /*
     * Evaluated constant value, filled by the `convert` units that compute it
     * (ADR-0017); zero/default for other kinds.
     *   int_value/int_type     : value and type of an integer constant
     *                            (§6.4.4.1), valid iff kind == QCC_TOKEN_INTEGER.
     *   float_value/float_type : value and type of a floating constant
     *                            (§6.4.4.2), valid iff kind == QCC_TOKEN_FLOATING.
     *   int_value/char_encoding: for a character constant (§6.4.4.4), valid iff
     *                            kind == QCC_TOKEN_CHAR — int_value is the value
     *                            and char_encoding the prefix (PLAIN is type int).
     *   str_data/str_len/char_encoding: for a string literal (§6.4.5), valid iff
     *                            kind == QCC_TOKEN_STRING — str_data points at
     *                            str_len code units (excluding the §6.4.5 ¶6 zero
     *                            terminator that follows them) of
     *                            qcc_encoding_unit_size(char_encoding) bytes each,
     *                            in target (little-endian) order; NULL otherwise.
     */
    uint64_t                 int_value;
    qcc_int_type             int_type;
    double                   float_value;
    qcc_float_type           float_type;
    qcc_char_encoding        char_encoding;
    const void              *str_data;
    size_t                   str_len;
} qcc_token;

/* Stable lowercase name of a token kind ("keyword", "integer-constant", …). */
const char *qcc_token_kind_name(qcc_token_kind kind);

/* Stable C spelling of an integer-constant type ("int", "unsigned long", …). */
const char *qcc_int_type_name(qcc_int_type type);

/* Stable C spelling of a floating-constant type ("double", "float", …). */
const char *qcc_float_type_name(qcc_float_type type);

/*
 * Canonical (primary) spelling of a punctuator — "[" for QCC_PUNCT_LBRACKET
 * even if the source wrote "<:". Static string, never NULL for valid input;
 * returns "?" for out-of-range values so the function is total.
 */
const char *qcc_punct_str(qcc_punct punct);

/*
 * Keyword lookup over a text span (NOT NUL-terminated): returns the keyword
 * whose spelling is exactly bytes[0..length), or QCC_KW_NONE. Used by phase-7
 * conversion; the span must already be splice-free (the caller reads it
 * through the lexer's spelling accessor).
 */
qcc_keyword qcc_keyword_lookup(const char *bytes, size_t length);

/* Spelling of a keyword ("_Static_assert", "while", …); "?" if out of range
   or QCC_KW_NONE (which has no spelling). */
const char *qcc_keyword_str(qcc_keyword keyword);

#endif /* QCC_TOKEN_TOKEN_H */
