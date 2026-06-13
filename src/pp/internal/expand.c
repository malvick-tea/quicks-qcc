/*
 * qcc — preprocessor internals: macro expansion (implementation).
 *
 * See expand.h for the contract and the §6.10.3.4 rationale. This realizes the
 * Prosser hide-set algorithm:
 *
 *   - object-like (§6.10.3 ¶9): replacement tokens inherit HS(name) ∪ {name}.
 *   - function-like (§6.10.3 ¶10-11): on seeing the macro name, look one token
 *     ahead (across newlines) for '('; if absent the name is not a call and is
 *     left alone. Otherwise collect the parenthesized, comma-separated arguments
 *     (with a variadic tail for '...'), then substitute (Prosser's subst):
 *       * a parameter not adjacent to # or ## is replaced by its *fully macro-
 *         expanded* argument (§6.10.3.1 ¶1);
 *       * '# param' stringizes the *unexpanded* argument (§6.10.3.2);
 *       * 'a ## b' pastes using *unexpanded* operands, empty arguments acting as
 *         placemarkers (§6.10.3.3);
 *     finally HS(name) ∩ HS(')') ∪ {name} is unioned into every output token.
 *   The substituted list is pushed onto the stream and rescanned.
 *
 * Argument macro-expansion runs the same expansion loop over a token-rooted
 * stream (qcc_pp_stream_init_tokens) so an argument is expanded "as if it formed
 * the rest of the preprocessing file" (§6.10.3.1 ¶1) — in isolation from tokens
 * after the call.
 */
#include "pp/internal/expand.h"

#include <stdlib.h>
#include <string.h>

#include "pp/internal/glue.h"
#include "pp/internal/hideset.h"

/* One collected macro argument: a borrowed (arena-owned) token run. */
typedef struct pp_arg {
    const qcc_ptok *toks;
    size_t          count;
} pp_arg;

/* A growable vector of arguments (the named ones), heap-backed and transient. */
typedef struct arg_vec {
    pp_arg *items;
    size_t  count;
    size_t  capacity;
} arg_vec;

/* A substitution-output token, carrying the placemarker flag (§6.10.3.3) that
   models an empty ## operand; placemarkers are dropped before output. */
typedef struct subst_tok {
    qcc_ptok tok;
    int      placemarker;
} subst_tok;

typedef struct subst_vec {
    subst_tok *items;
    size_t     count;
    size_t     capacity;
} subst_vec;

/* Forward declaration: argument pre-expansion and substitution recurse. */
static qcc_status expand_token_list(qcc_pp *pp, const qcc_ptok *toks,
                                    size_t count, qcc_ptok_list *out);

/* Private helpers: small vectors. */

static int arg_vec_push(arg_vec *v, pp_arg a)
{
    if (v->count == v->capacity) {
        size_t cap = (v->capacity == 0) ? 8u : v->capacity * 2u;
        pp_arg *grown = (pp_arg *)realloc(v->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        v->items    = grown;
        v->capacity = cap;
    }
    v->items[v->count++] = a;
    return 1;
}

static void arg_vec_free(arg_vec *v)
{
    free(v->items);
    v->items = NULL;
    v->count = v->capacity = 0;
}

static int subst_push(subst_vec *v, qcc_ptok tok, int placemarker)
{
    if (v->count == v->capacity) {
        size_t cap = (v->capacity == 0) ? 16u : v->capacity * 2u;
        subst_tok *grown = (subst_tok *)realloc(v->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        v->items    = grown;
        v->capacity = cap;
    }
    v->items[v->count].tok         = tok;
    v->items[v->count].placemarker = placemarker;
    v->count += 1;
    return 1;
}

static void subst_free(subst_vec *v)
{
    free(v->items);
    v->items = NULL;
    v->count = v->capacity = 0;
}

static int is_punct(const qcc_ptok *t, qcc_punct p)
{
    return t->kind == QCC_PP_TOKEN_PUNCT && t->punct == p;
}

/* Copy a token list into the arena. An empty list yields {NULL, 0}. */
static qcc_status arena_dup_list(qcc_pp *pp, const qcc_ptok_list *list, pp_arg *out)
{
    out->count = list->count;
    out->toks  = NULL;
    if (list->count == 0) {
        return QCC_OK;
    }
    out->toks = (const qcc_ptok *)qcc_arena_memdup(
        &pp->arena, list->items, list->count * sizeof(qcc_ptok), _Alignof(qcc_ptok));
    return (out->toks != NULL) ? QCC_OK : QCC_ERR_OUT_OF_MEMORY;
}

/*
 * Index of the parameter `spelling` names: 0..param_count-1 for a named
 * parameter, param_count for __VA_ARGS__ in a variadic macro, or -1 if not a
 * parameter. `spelling` is interned, so named parameters compare by pointer.
 */
static int param_index(const qcc_macro *macro, const char *spelling)
{
    for (size_t i = 0; i < macro->param_count; ++i) {
        if (macro->params[i] == spelling) {
            return (int)i;
        }
    }
    if (macro->is_variadic && strcmp(spelling, "__VA_ARGS__") == 0) {
        return (int)macro->param_count;
    }
    return -1;
}

/* Object-like expansion (§6.10.3 ¶9): see header. */
static qcc_status expand_object(qcc_pp *pp, qcc_pp_stream *stream,
                                const qcc_ptok *name, const qcc_macro *macro)
{
    const qcc_hideset *new_hs = NULL;
    qcc_status st = qcc_hideset_add(&pp->arena, name->hideset, name->spelling,
                                    &new_hs);
    if (st != QCC_OK) {
        return st;
    }

    size_t n = macro->replacement_count;
    if (n == 0) {
        return QCC_OK;
    }
    qcc_ptok *copy = (qcc_ptok *)qcc_arena_alloc(&pp->arena, n * sizeof(*copy),
                                                 _Alignof(qcc_ptok));
    if (copy == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < n; ++i) {
        copy[i] = macro->replacement[i];
        const qcc_hideset *u = NULL;
        st = qcc_hideset_union(&pp->arena, copy[i].hideset, new_hs, &u);
        if (st != QCC_OK) {
            return st;
        }
        copy[i].hideset       = u;
        copy[i].at_line_start = 0;
    }
    copy[0].leading_space = name->leading_space;
    return qcc_pp_stream_push_tokens(stream, copy, n);
}

/* Run the full expansion loop over a token sequence in isolation, appending the
   result to `out`. Used to macro-expand an argument (§6.10.3.1 ¶1). */
static qcc_status expand_token_list(qcc_pp *pp, const qcc_ptok *toks,
                                    size_t count, qcc_ptok_list *out)
{
    qcc_pp_stream sub;
    qcc_status    st = qcc_pp_stream_init_tokens(&sub, pp, toks, count);
    if (st != QCC_OK) {
        return st;
    }
    for (;;) {
        qcc_ptok t;
        int      fe;
        st = qcc_pp_stream_next(&sub, &t, &fe);
        if (st != QCC_OK) {
            break;
        }
        if (t.kind == QCC_PP_TOKEN_EOF) {
            break;
        }
        if (t.kind == QCC_PP_TOKEN_IDENTIFIER) {
            qcc_macro *m = qcc_macro_lookup(pp->macros, t.spelling);
            if (m != NULL && !qcc_hideset_contains(t.hideset, t.spelling)) {
                int expanded = 0;
                st = qcc_pp_expand(pp, &sub, &t, m, &expanded);
                if (st != QCC_OK) {
                    break;
                }
                if (expanded) {
                    continue;
                }
            }
        }
        st = qcc_ptok_list_push(out, &t);
        if (st != QCC_OK) {
            break;
        }
    }
    qcc_pp_stream_dispose(&sub);
    return st;
}

/*
 * Collect a function-like macro's arguments, the '(' already consumed. Fills
 * `named` (one entry per top-level, comma-separated argument up to the variadic
 * cut) and, for a variadic macro, *vararg (the raw tail). Captures the closing
 * ')' into *rparen for the hide-set computation. Sets *valid = 0 (after a
 * diagnostic) on an unterminated list. Returns QCC_OK or a hard fault.
 */
static qcc_status collect_args(qcc_pp *pp, qcc_pp_stream *stream,
                               const qcc_macro *macro, const qcc_ptok *name,
                               arg_vec *named, pp_arg *vararg, int *has_vararg,
                               qcc_ptok *rparen, int *valid)
{
    *valid       = 1;
    *has_vararg  = 0;
    vararg->toks = NULL;
    vararg->count = 0;
    *rparen      = *name; /* Fallback provenance if the list is unterminated. */

    qcc_ptok_list cur;
    qcc_ptok_list_init(&cur);
    int        depth         = 0;
    int        collecting_va = (macro->is_variadic && macro->param_count == 0);
    int        pending_space = 0;
    qcc_status st            = QCC_OK;

    for (;;) {
        qcc_ptok t;
        int      fe;
        st = qcc_pp_stream_next(stream, &t, &fe);
        if (st != QCC_OK) {
            break;
        }

        if (t.kind == QCC_PP_TOKEN_NEWLINE) {
            pending_space = 1; /* A newline in the list is just white space. */
            continue;
        }
        if (t.kind == QCC_PP_TOKEN_EOF) {
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name->source, name->offset,
                          (name->spelling_len ? name->spelling_len : 1u),
                          "unterminated argument list invoking macro '%s'",
                          name->spelling);
            (void)qcc_pp_stream_unget(stream, &t, fe); /* Let the caller see EOF. */
            *valid = 0;
            break;
        }

        if (depth == 0 && is_punct(&t, QCC_PUNCT_RPAREN)) {
            *rparen = t;
            if (collecting_va) {
                st = arena_dup_list(pp, &cur, vararg);
                *has_vararg = 1;
            } else {
                pp_arg g;
                st = arena_dup_list(pp, &cur, &g);
                if (st == QCC_OK && !arg_vec_push(named, g)) {
                    st = QCC_ERR_OUT_OF_MEMORY;
                }
            }
            break;
        }

        if (depth == 0 && !collecting_va && is_punct(&t, QCC_PUNCT_COMMA)) {
            pp_arg g;
            st = arena_dup_list(pp, &cur, &g);
            if (st != QCC_OK) {
                break;
            }
            if (!arg_vec_push(named, g)) {
                st = QCC_ERR_OUT_OF_MEMORY;
                break;
            }
            qcc_ptok_list_clear(&cur);
            pending_space = 0;
            if (macro->is_variadic && named->count == macro->param_count) {
                collecting_va = 1; /* Remaining tokens (incl. commas) are varargs.*/
            }
            continue;
        }

        if (is_punct(&t, QCC_PUNCT_LPAREN)) {
            depth += 1;
        } else if (is_punct(&t, QCC_PUNCT_RPAREN)) {
            depth -= 1; /* depth > 0 here: a nested ')'. */
        }
        if (pending_space) {
            t.leading_space = 1;
            pending_space    = 0;
        }
        st = qcc_ptok_list_push(&cur, &t);
        if (st != QCC_OK) {
            break;
        }
    }

    qcc_ptok_list_dispose(&cur);
    return st;
}

/* Glue a right-hand sequence onto the last token of `os` (§6.10.3.3). An empty
   rhs is a placemarker; placemarkers paste away. Returns QCC_OK or a fault. */
static qcc_status glue_rhs(qcc_pp *pp, subst_vec *os, const qcc_ptok *rhs,
                           size_t rhs_count, const qcc_ptok *anchor)
{
    if (rhs_count == 0) {
        /* X ## <placemarker> = X; <pm> ## <pm> = <pm>. If os is empty (## at
           start, normally rejected at define time), introduce a placemarker. */
        if (os->count == 0) {
            qcc_ptok pm;
            memset(&pm, 0, sizeof(pm));
            return subst_push(os, pm, 1) ? QCC_OK : QCC_ERR_OUT_OF_MEMORY;
        }
        return QCC_OK;
    }

    if (os->count == 0) {
        for (size_t k = 0; k < rhs_count; ++k) {
            if (!subst_push(os, rhs[k], 0)) {
                return QCC_ERR_OUT_OF_MEMORY;
            }
        }
        return QCC_OK;
    }

    subst_tok *left = &os->items[os->count - 1];
    if (left->placemarker) {
        left->tok         = rhs[0]; /* <placemarker> ## R0 = R0. */
        left->placemarker = 0;
    } else {
        qcc_ptok pasted;
        int      ok = 0;
        qcc_status st = qcc_pp_paste(pp, &left->tok, &rhs[0], anchor, &pasted, &ok);
        if (st != QCC_OK) {
            return st;
        }
        left->tok = pasted; /* On failure paste yields `left` (diagnosed). */
    }
    for (size_t k = 1; k < rhs_count; ++k) {
        if (!subst_push(os, rhs[k], 0)) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
    }
    return QCC_OK;
}

/*
 * Prosser subst over the replacement list; produces the substituted token array
 * (placemarkers dropped, hide sets applied) via *out_toks / *out_count. `args`
 * has `nargs` slots (named parameters then the optional __VA_ARGS__).
 */
static qcc_status substitute(qcc_pp *pp, const qcc_macro *macro,
                             const pp_arg *args, size_t nargs,
                             const qcc_ptok *name, const qcc_ptok *rparen,
                             const qcc_ptok **out_toks, size_t *out_count)
{
    (void)nargs;
    const qcc_ptok *R = macro->replacement;
    size_t          n = macro->replacement_count;
    subst_vec       os = { NULL, 0, 0 };
    qcc_status      st = QCC_OK;
    size_t          i  = 0;

    while (i < n) {
        const qcc_ptok *T = &R[i];

        /* '# parameter' -> stringize the unexpanded argument. */
        if (macro->is_function_like && is_punct(T, QCC_PUNCT_HASH) && i + 1 < n &&
            R[i + 1].kind == QCC_PP_TOKEN_IDENTIFIER) {
            int pi = param_index(macro, R[i + 1].spelling);
            if (pi >= 0) {
                qcc_ptok s;
                st = qcc_pp_stringize(pp, args[pi].toks, args[pi].count, name, &s);
                if (st != QCC_OK || !subst_push(&os, s, 0)) {
                    if (st == QCC_OK) st = QCC_ERR_OUT_OF_MEMORY;
                    break;
                }
                i += 2;
                continue;
            }
        }

        /* '## operand' -> paste with the last output token. */
        if (is_punct(T, QCC_PUNCT_HASH_HASH) && i + 1 < n) {
            const qcc_ptok *rt = &R[i + 1];
            int pi = (rt->kind == QCC_PP_TOKEN_IDENTIFIER)
                         ? param_index(macro, rt->spelling) : -1;
            if (pi >= 0) {
                st = glue_rhs(pp, &os, args[pi].toks, args[pi].count, T);
            } else {
                st = glue_rhs(pp, &os, rt, 1, T);
            }
            if (st != QCC_OK) {
                break;
            }
            i += 2;
            continue;
        }

        /* 'parameter ##' -> left operand: push the unexpanded argument (or a
           placemarker when empty), leaving the paste to the next iteration. */
        if (T->kind == QCC_PP_TOKEN_IDENTIFIER && i + 1 < n &&
            is_punct(&R[i + 1], QCC_PUNCT_HASH_HASH)) {
            int pi = param_index(macro, T->spelling);
            if (pi >= 0) {
                if (args[pi].count == 0) {
                    qcc_ptok pm;
                    memset(&pm, 0, sizeof(pm));
                    if (!subst_push(&os, pm, 1)) {
                        st = QCC_ERR_OUT_OF_MEMORY;
                        break;
                    }
                } else {
                    for (size_t k = 0; k < args[pi].count; ++k) {
                        if (!subst_push(&os, args[pi].toks[k], 0)) {
                            st = QCC_ERR_OUT_OF_MEMORY;
                            break;
                        }
                    }
                    if (st != QCC_OK) {
                        break;
                    }
                }
                i += 1;
                continue;
            }
        }

        /* A parameter not adjacent to # or ## -> fully macro-expanded argument. */
        if (T->kind == QCC_PP_TOKEN_IDENTIFIER) {
            int pi = param_index(macro, T->spelling);
            if (pi >= 0) {
                qcc_ptok_list exp;
                qcc_ptok_list_init(&exp);
                st = expand_token_list(pp, args[pi].toks, args[pi].count, &exp);
                if (st == QCC_OK) {
                    for (size_t k = 0; k < exp.count; ++k) {
                        qcc_ptok et = exp.items[k];
                        if (k == 0) {
                            et.leading_space = T->leading_space;
                        }
                        if (!subst_push(&os, et, 0)) {
                            st = QCC_ERR_OUT_OF_MEMORY;
                            break;
                        }
                    }
                }
                qcc_ptok_list_dispose(&exp);
                if (st != QCC_OK) {
                    break;
                }
                i += 1;
                continue;
            }
        }

        /* Ordinary replacement token. */
        if (!subst_push(&os, *T, 0)) {
            st = QCC_ERR_OUT_OF_MEMORY;
            break;
        }
        i += 1;
    }

    if (st != QCC_OK) {
        subst_free(&os);
        return st;
    }

    /* HS' = (HS(name) ∩ HS(')')) ∪ {name}: the function-like hide set. */
    const qcc_hideset *inter = NULL;
    st = qcc_hideset_intersect(&pp->arena, name->hideset, rparen->hideset, &inter);
    const qcc_hideset *new_hs = NULL;
    if (st == QCC_OK) {
        st = qcc_hideset_add(&pp->arena, inter, name->spelling, &new_hs);
    }
    if (st != QCC_OK) {
        subst_free(&os);
        return st;
    }

    /* Drop placemarkers; apply the hide set; materialize the final array. */
    size_t kept = 0;
    for (size_t k = 0; k < os.count; ++k) {
        if (!os.items[k].placemarker) {
            kept += 1;
        }
    }
    qcc_ptok *final_toks = NULL;
    if (kept != 0) {
        final_toks = (qcc_ptok *)qcc_arena_alloc(&pp->arena, kept * sizeof(qcc_ptok),
                                                 _Alignof(qcc_ptok));
        if (final_toks == NULL) {
            subst_free(&os);
            return QCC_ERR_OUT_OF_MEMORY;
        }
        size_t j = 0;
        for (size_t k = 0; k < os.count; ++k) {
            if (os.items[k].placemarker) {
                continue;
            }
            qcc_ptok tk = os.items[k].tok;
            const qcc_hideset *u = NULL;
            st = qcc_hideset_union(&pp->arena, tk.hideset, new_hs, &u);
            if (st != QCC_OK) {
                subst_free(&os);
                return st;
            }
            tk.hideset       = u;
            tk.at_line_start = 0;
            final_toks[j++]  = tk;
        }
        final_toks[0].leading_space = name->leading_space;
    }

    subst_free(&os);
    *out_toks  = final_toks;
    *out_count = kept;
    return QCC_OK;
}

/* Function-like expansion (§6.10.3 ¶10-11). */
static qcc_status expand_function(qcc_pp *pp, qcc_pp_stream *stream,
                                  const qcc_ptok *name, const qcc_macro *macro,
                                  int *out_expanded)
{
    /* Look ahead (across newlines) for the '(' of an invocation. */
    qcc_ptok t;
    int      fe;
    for (;;) {
        qcc_status st = qcc_pp_stream_next(stream, &t, &fe);
        if (st != QCC_OK) {
            return st;
        }
        if (t.kind != QCC_PP_TOKEN_NEWLINE) {
            break;
        }
    }
    if (!is_punct(&t, QCC_PUNCT_LPAREN)) {
        *out_expanded = 0;            /* Not an invocation; leave the name. */
        return qcc_pp_stream_unget(stream, &t, fe);
    }

    arg_vec  named = { NULL, 0, 0 };
    pp_arg   vararg = { NULL, 0 };
    int      has_vararg = 0;
    int      valid = 1;
    qcc_ptok rparen;
    qcc_status st = collect_args(pp, stream, macro, name, &named, &vararg,
                                 &has_vararg, &rparen, &valid);
    if (st != QCC_OK) {
        arg_vec_free(&named);
        return st;
    }
    if (!valid) {
        arg_vec_free(&named);
        *out_expanded = 0;
        return QCC_OK;
    }

    /* Argument-count check (§6.10.3 ¶4). */
    int count_ok = 1;
    if (!macro->is_variadic) {
        if (macro->param_count == 0) {
            if (!(named.count == 1 && named.items[0].count == 0)) {
                qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name->source,
                              name->offset, (name->spelling_len ? name->spelling_len : 1u),
                              "too many arguments provided to macro '%s'",
                              name->spelling);
                count_ok = 0;
            }
        } else if (named.count != macro->param_count) {
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name->source, name->offset,
                          (name->spelling_len ? name->spelling_len : 1u),
                          "macro '%s' passed %zu arguments, but takes %zu",
                          name->spelling, named.count, macro->param_count);
            count_ok = 0;
        }
    } else if (named.count != macro->param_count) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name->source, name->offset,
                      (name->spelling_len ? name->spelling_len : 1u),
                      "macro '%s' requires at least %zu arguments, but %zu given",
                      name->spelling, macro->param_count, named.count);
        count_ok = 0;
    }
    if (!count_ok) {
        arg_vec_free(&named);
        *out_expanded = 0;
        return QCC_OK;
    }

    /* Build the flat argument array: named parameters then __VA_ARGS__. */
    size_t  nargs = macro->param_count + (macro->is_variadic ? 1u : 0u);
    pp_arg *args  = NULL;
    if (nargs != 0) {
        args = (pp_arg *)qcc_arena_alloc(&pp->arena, nargs * sizeof(pp_arg),
                                         _Alignof(pp_arg));
        if (args == NULL) {
            arg_vec_free(&named);
            return QCC_ERR_OUT_OF_MEMORY;
        }
        for (size_t k = 0; k < macro->param_count; ++k) {
            args[k] = named.items[k];
        }
        if (macro->is_variadic) {
            args[macro->param_count] = vararg; /* Empty if no varargs were given. */
        }
    }

    const qcc_ptok *result      = NULL;
    size_t          result_count = 0;
    st = substitute(pp, macro, args, nargs, name, &rparen, &result, &result_count);
    arg_vec_free(&named);
    if (st != QCC_OK) {
        return st;
    }

    st = qcc_pp_stream_push_tokens(stream, result, result_count);
    if (st != QCC_OK) {
        return st;
    }
    *out_expanded = 1;
    return QCC_OK;
}

qcc_status qcc_pp_expand(qcc_pp *pp, qcc_pp_stream *stream, const qcc_ptok *name,
                         const qcc_macro *macro, int *out_expanded)
{
    if (pp == NULL || stream == NULL || name == NULL || macro == NULL ||
        out_expanded == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out_expanded = 0;

    if (macro->is_function_like) {
        return expand_function(pp, stream, name, macro, out_expanded);
    }

    qcc_status st = expand_object(pp, stream, name, macro);
    if (st != QCC_OK) {
        return st;
    }
    *out_expanded = 1;
    return QCC_OK;
}
