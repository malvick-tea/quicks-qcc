/*
 * qcc — lexer internals: punctuator scanner, implementation.
 *
 * One switch, longest spelling first. The three lookahead characters are
 * read through the cursor up front: punctuators are at most four characters
 * ("%:%:"), and reading logically means a splice between the two '<' of
 * "<<" is already invisible here.
 */
#include "lexer/internal/punct.h"

#include "lexer/internal/cursor.h"

qcc_status qcc_lx_scan_punct_or_other(const qcc_source *src,
                                      qcc_diag_sink *diags, size_t start,
                                      qcc_lx_scan *out)
{
    size_t p1, n1, p2, n2, p3, n3;
    char c1 = qcc_lx_at(src, start, &p1, &n1);
    char c2 = qcc_lx_at(src, n1, &p2, &n2);
    char c3 = qcc_lx_at(src, n2, &p3, &n3);

    qcc_punct punct;
    size_t    end;

    switch (c1) {
    case '[': punct = QCC_PUNCT_LBRACKET; end = n1; break;
    case ']': punct = QCC_PUNCT_RBRACKET; end = n1; break;
    case '(': punct = QCC_PUNCT_LPAREN;   end = n1; break;
    case ')': punct = QCC_PUNCT_RPAREN;   end = n1; break;
    case '{': punct = QCC_PUNCT_LBRACE;   end = n1; break;
    case '}': punct = QCC_PUNCT_RBRACE;   end = n1; break;
    case ';': punct = QCC_PUNCT_SEMI;     end = n1; break;
    case ',': punct = QCC_PUNCT_COMMA;    end = n1; break;
    case '~': punct = QCC_PUNCT_TILDE;    end = n1; break;

    case '.':
        /* "..." is one token; ".." is DOT then DOT (no two-dot token). */
        if (c2 == '.' && c3 == '.') { punct = QCC_PUNCT_ELLIPSIS; end = n3; }
        else                        { punct = QCC_PUNCT_DOT;      end = n1; }
        break;

    case '-':
        if      (c2 == '>') { punct = QCC_PUNCT_ARROW;       end = n2; }
        else if (c2 == '-') { punct = QCC_PUNCT_MINUS_MINUS; end = n2; }
        else if (c2 == '=') { punct = QCC_PUNCT_MINUS_EQ;    end = n2; }
        else                { punct = QCC_PUNCT_MINUS;       end = n1; }
        break;

    case '+':
        if      (c2 == '+') { punct = QCC_PUNCT_PLUS_PLUS; end = n2; }
        else if (c2 == '=') { punct = QCC_PUNCT_PLUS_EQ;   end = n2; }
        else                { punct = QCC_PUNCT_PLUS;      end = n1; }
        break;

    case '&':
        if      (c2 == '&') { punct = QCC_PUNCT_AMP_AMP; end = n2; }
        else if (c2 == '=') { punct = QCC_PUNCT_AMP_EQ;  end = n2; }
        else                { punct = QCC_PUNCT_AMP;     end = n1; }
        break;

    case '|':
        if      (c2 == '|') { punct = QCC_PUNCT_PIPE_PIPE; end = n2; }
        else if (c2 == '=') { punct = QCC_PUNCT_PIPE_EQ;   end = n2; }
        else                { punct = QCC_PUNCT_PIPE;      end = n1; }
        break;

    case '!':
        if (c2 == '=') { punct = QCC_PUNCT_BANG_EQ; end = n2; }
        else           { punct = QCC_PUNCT_BANG;    end = n1; }
        break;

    case '*':
        if (c2 == '=') { punct = QCC_PUNCT_STAR_EQ; end = n2; }
        else           { punct = QCC_PUNCT_STAR;    end = n1; }
        break;

    case '/':
        /* Comments were consumed by the driver; only '/' and '/=' remain. */
        if (c2 == '=') { punct = QCC_PUNCT_SLASH_EQ; end = n2; }
        else           { punct = QCC_PUNCT_SLASH;    end = n1; }
        break;

    case '=':
        if (c2 == '=') { punct = QCC_PUNCT_EQ_EQ; end = n2; }
        else           { punct = QCC_PUNCT_EQ;    end = n1; }
        break;

    case '^':
        if (c2 == '=') { punct = QCC_PUNCT_CARET_EQ; end = n2; }
        else           { punct = QCC_PUNCT_CARET;    end = n1; }
        break;

    case '<':
        if (c2 == '<') {
            if (c3 == '=') { punct = QCC_PUNCT_LSHIFT_EQ; end = n3; }
            else           { punct = QCC_PUNCT_LSHIFT;    end = n2; }
        }
        else if (c2 == '=') { punct = QCC_PUNCT_LE;       end = n2; }
        else if (c2 == ':') { punct = QCC_PUNCT_LBRACKET; end = n2; } /* <: */
        else if (c2 == '%') { punct = QCC_PUNCT_LBRACE;   end = n2; } /* <% */
        else                { punct = QCC_PUNCT_LT;       end = n1; }
        break;

    case '>':
        if (c2 == '>') {
            if (c3 == '=') { punct = QCC_PUNCT_RSHIFT_EQ; end = n3; }
            else           { punct = QCC_PUNCT_RSHIFT;    end = n2; }
        }
        else if (c2 == '=') { punct = QCC_PUNCT_GE; end = n2; }
        else                { punct = QCC_PUNCT_GT; end = n1; }
        break;

    case '%':
        if      (c2 == '=') { punct = QCC_PUNCT_PERCENT_EQ; end = n2; }
        else if (c2 == '>') { punct = QCC_PUNCT_RBRACE;     end = n2; } /* %> */
        else if (c2 == ':') {
            /* %: is #; %:%: is the four-character ## (§6.4.6 ¶3). */
            size_t p4, n4;
            char   c4 = qcc_lx_at(src, n3, &p4, &n4);
            if (c3 == '%' && c4 == ':') { punct = QCC_PUNCT_HASH_HASH; end = n4; }
            else                        { punct = QCC_PUNCT_HASH;      end = n2; }
        }
        else { punct = QCC_PUNCT_PERCENT; end = n1; }
        break;

    case ':':
        if (c2 == '>') { punct = QCC_PUNCT_RBRACKET; end = n2; } /* :> */
        else           { punct = QCC_PUNCT_COLON;    end = n1; }
        break;

    case '?': {
        qcc_status st = qcc_lx_warn_trigraph(src, diags, start);
        if (st != QCC_OK) {
            return st;
        }
        punct = QCC_PUNCT_QUESTION;
        end   = n1;
        break;
    }

    case '#':
        if (c2 == '#') { punct = QCC_PUNCT_HASH_HASH; end = n2; }
        else           { punct = QCC_PUNCT_HASH;      end = n1; }
        break;

    default:
        /* Not a punctuator: an "other" pp-token, one character long
           (§6.4 ¶1) — @, $, `, a raw NUL byte, bytes >= 0x80. */
        out->kind  = QCC_PP_TOKEN_OTHER;
        out->punct = (qcc_punct)0;
        out->end   = n1;
        return QCC_OK;
    }

    out->kind  = QCC_PP_TOKEN_PUNCT;
    out->punct = punct;
    out->end   = end;
    return QCC_OK;
}
