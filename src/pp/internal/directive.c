/*
 * qcc — preprocessor internals: directive parsing and dispatch (implementation).
 *
 * See directive.h for the contract. This step implements #define (object- and
 * function-like, including variadic) and #undef, plus the null directive
 * (§6.10.7). Unimplemented directives are diagnosed and skipped; they land in
 * later steps (conditionals, #include, #line/#error/#pragma).
 *
 * Spec anchors: §6.10.3 (macro definition), §6.10.3 ¶5 (__VA_ARGS__ / reserved
 * parameter rules), §6.10.3.2 ¶1 (# must precede a parameter), §6.10.3.3 ¶1
 * (## not at either end of a replacement list), §6.10.3.5 (#undef).
 */
#include "pp/internal/directive.h"

#include <stdlib.h>
#include <string.h>

#include "diag/diag.h"
#include "pp/internal/ceval.h"
#include "pp/internal/cond.h"
#include "pp/internal/macro.h"

/* A growable vector of interned parameter-name pointers (no fixed limit). */
typedef struct name_vec {
    const char **items;
    size_t       count;
    size_t       capacity;
} name_vec;

/* Private helpers. */

/* Span length to underline for a token in diagnostics (at least one column). */
static size_t tok_span(const qcc_ptok *tok)
{
    return (tok->spelling_len != 0) ? tok->spelling_len : 1u;
}

static int name_vec_contains(const name_vec *v, const char *name)
{
    for (size_t i = 0; i < v->count; ++i) {
        if (v->items[i] == name) { /* Interned: pointer identity. */
            return 1;
        }
    }
    return 0;
}

/* Append to a name_vec, growing geometrically. Returns 0 on OOM. */
static int name_vec_push(name_vec *v, const char *name)
{
    if (v->count == v->capacity) {
        size_t cap = (v->capacity == 0) ? 8u : v->capacity * 2u;
        const char **grown =
            (const char **)realloc((void *)v->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        v->items    = grown;
        v->capacity = cap;
    }
    v->items[v->count++] = name;
    return 1;
}

static void name_vec_free(name_vec *v)
{
    free((void *)v->items);
    v->items    = NULL;
    v->count    = 0;
    v->capacity = 0;
}

static int is_punct(const qcc_ptok *t, qcc_punct p)
{
    return t->kind == QCC_PP_TOKEN_PUNCT && t->punct == p;
}

static int is_line_end(const qcc_ptok *t)
{
    return t->kind == QCC_PP_TOKEN_NEWLINE || t->kind == QCC_PP_TOKEN_EOF;
}

/* Read and discard the rest of the logical line (through its newline/EOF). */
static qcc_status skip_to_line_end(qcc_pp_stream *stream)
{
    for (;;) {
        qcc_ptok t;
        int      fe;
        qcc_status st = qcc_pp_stream_next(stream, &t, &fe);
        if (st != QCC_OK) {
            return st;
        }
        if (is_line_end(&t)) {
            return QCC_OK;
        }
    }
}

/* Is `name` (interned) one of this macro's parameters (or __VA_ARGS__ when
   variadic)? Used to check the §6.10.3.2 constraint on '#'. */
static int is_parameter(const char *name, const name_vec *params, int variadic)
{
    if (name_vec_contains(params, name)) {
        return 1;
    }
    return variadic && strcmp(name, "__VA_ARGS__") == 0;
}

/*
 * Parse the parameter list of a function-like macro, having just consumed the
 * '('. Fills *params and *variadic. Sets *valid to 0 (and skips to line end)
 * on a malformed list, after emitting a diagnostic. Returns QCC_OK or a hard
 * fault.
 */
static qcc_status parse_params(qcc_pp *pp, qcc_pp_stream *stream,
                               name_vec *params, int *variadic, int *valid)
{
    qcc_ptok t;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &t, &fe);
    if (st != QCC_OK) {
        return st;
    }
    if (is_punct(&t, QCC_PUNCT_RPAREN)) {
        return QCC_OK; /* Empty parameter list: NAME(). */
    }

    for (;;) {
        if (is_punct(&t, QCC_PUNCT_ELLIPSIS)) {
            *variadic = 1;
            st = qcc_pp_stream_next(stream, &t, &fe);
            if (st != QCC_OK) {
                return st;
            }
            if (!is_punct(&t, QCC_PUNCT_RPAREN)) {
                qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t.source, t.offset,
                              tok_span(&t), "expected ')' after '...' in macro "
                              "parameter list");
                *valid = 0;
                return skip_to_line_end(stream);
            }
            return QCC_OK;
        }

        if (t.kind != QCC_PP_TOKEN_IDENTIFIER) {
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t.source, t.offset,
                          tok_span(&t), "expected parameter name in macro "
                          "parameter list");
            *valid = 0;
            return skip_to_line_end(stream);
        }
        if (name_vec_contains(params, t.spelling)) {
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t.source, t.offset,
                          tok_span(&t), "duplicate macro parameter '%s'",
                          t.spelling);
            *valid = 0;
            return skip_to_line_end(stream);
        }
        if (strcmp(t.spelling, "__VA_ARGS__") == 0) {
            /* §6.10.3 ¶5: __VA_ARGS__ is reserved; it cannot be a parameter. */
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t.source, t.offset,
                          tok_span(&t), "'__VA_ARGS__' cannot be used as a macro "
                          "parameter name");
            *valid = 0;
            return skip_to_line_end(stream);
        }
        if (!name_vec_push(params, t.spelling)) {
            return QCC_ERR_OUT_OF_MEMORY;
        }

        st = qcc_pp_stream_next(stream, &t, &fe);
        if (st != QCC_OK) {
            return st;
        }
        if (is_punct(&t, QCC_PUNCT_RPAREN)) {
            return QCC_OK;
        }
        if (is_punct(&t, QCC_PUNCT_COMMA)) {
            st = qcc_pp_stream_next(stream, &t, &fe); /* Token after the comma. */
            if (st != QCC_OK) {
                return st;
            }
            continue;
        }
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t.source, t.offset, tok_span(&t),
                      "expected ',' or ')' in macro parameter list");
        *valid = 0;
        return skip_to_line_end(stream);
    }
}

/* Collect replacement tokens up to (not including) the line end into `repl`. */
static qcc_status collect_replacement(qcc_pp_stream *stream, qcc_ptok_list *repl)
{
    for (;;) {
        qcc_ptok t;
        int      fe;
        qcc_status st = qcc_pp_stream_next(stream, &t, &fe);
        if (st != QCC_OK) {
            return st;
        }
        if (is_line_end(&t)) {
            return QCC_OK;
        }
        st = qcc_ptok_list_push(repl, &t);
        if (st != QCC_OK) {
            return st;
        }
    }
}

/*
 * Check the replacement-list constraints (§6.10.3.2 ¶1, §6.10.3.3 ¶1). Emits
 * diagnostics; does not fail the build (the macro is still stored best-effort).
 */
static void validate_replacement(qcc_pp *pp, int func_like, int variadic,
                                  const name_vec *params, const qcc_ptok_list *repl)
{
    size_t n = repl->count;
    if (n == 0) {
        return;
    }

    /* §6.10.3.3 ¶1: ## shall not be at the beginning or end (either form). */
    if (is_punct(&repl->items[0], QCC_PUNCT_HASH_HASH)) {
        const qcc_ptok *t = &repl->items[0];
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t->source, t->offset,
                      tok_span(t), "'##' cannot appear at the beginning of a "
                      "macro replacement list");
    }
    if (is_punct(&repl->items[n - 1], QCC_PUNCT_HASH_HASH)) {
        const qcc_ptok *t = &repl->items[n - 1];
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t->source, t->offset,
                      tok_span(t), "'##' cannot appear at the end of a macro "
                      "replacement list");
    }

    /* §6.10.3.2 ¶1: in a function-like macro, each # must be followed by a
       parameter. (In an object-like macro, # is an ordinary token.) */
    if (func_like) {
        for (size_t i = 0; i < n; ++i) {
            if (!is_punct(&repl->items[i], QCC_PUNCT_HASH)) {
                continue;
            }
            int ok = (i + 1 < n) &&
                     repl->items[i + 1].kind == QCC_PP_TOKEN_IDENTIFIER &&
                     is_parameter(repl->items[i + 1].spelling, params, variadic);
            if (!ok) {
                const qcc_ptok *t = &repl->items[i];
                qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, t->source, t->offset,
                              tok_span(t), "'#' is not followed by a macro "
                              "parameter");
            }
        }
    }
}

/*
 * Build the macro record in the arena from the parsed pieces. Returns the
 * record, or NULL on OOM. The first replacement token's leading-space is
 * normalized off (separation from the name is not part of the replacement's
 * internal spacing, and §6.10.3 ¶1 treats all such separations as identical).
 */
static qcc_macro *build_macro(qcc_pp *pp, const qcc_ptok *name, int func_like,
                              int variadic, const name_vec *params,
                              qcc_ptok_list *repl)
{
    qcc_macro *m =
        (qcc_macro *)qcc_arena_alloc(&pp->arena, sizeof(*m), _Alignof(qcc_macro));
    if (m == NULL) {
        return NULL;
    }
    m->name              = name->spelling;
    m->is_function_like  = func_like ? 1u : 0u;
    m->is_variadic       = variadic ? 1u : 0u;
    m->builtin           = QCC_MACRO_BUILTIN_NONE; /* A user macro, not builtin. */
    m->param_count       = params->count;
    m->params            = NULL;
    m->replacement       = NULL;
    m->replacement_count = repl->count;
    m->def_source        = name->source;
    m->def_offset        = name->offset;

    if (params->count != 0) {
        m->params = (const char **)qcc_arena_memdup(
            &pp->arena, params->items, params->count * sizeof(*params->items),
            _Alignof(const char *));
        if (m->params == NULL) {
            return NULL;
        }
    }
    if (repl->count != 0) {
        repl->items[0].leading_space = 0;
        m->replacement = (const qcc_ptok *)qcc_arena_memdup(
            &pp->arena, repl->items, repl->count * sizeof(*repl->items),
            _Alignof(qcc_ptok));
        if (m->replacement == NULL) {
            return NULL;
        }
    }
    return m;
}

static qcc_status do_define(qcc_pp *pp, qcc_pp_stream *stream)
{
    qcc_ptok name;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &name, &fe);
    if (st != QCC_OK) {
        return st;
    }
    if (name.kind != QCC_PP_TOKEN_IDENTIFIER) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name.source, name.offset,
                      tok_span(&name), "macro name must be an identifier");
        return is_line_end(&name) ? QCC_OK : skip_to_line_end(stream);
    }

    /* The token right after the name decides the form: '(' with no intervening
       white space -> function-like (§6.10.3 ¶3); otherwise object-like and this
       token is the first of the replacement list. */
    qcc_ptok after;
    st = qcc_pp_stream_next(stream, &after, &fe);
    if (st != QCC_OK) {
        return st;
    }
    int func_like =
        is_punct(&after, QCC_PUNCT_LPAREN) && after.leading_space == 0;

    name_vec      params = { NULL, 0, 0 };
    int           variadic = 0;
    int           valid = 1;
    qcc_ptok_list repl;
    qcc_ptok_list_init(&repl);

    if (func_like) {
        st = parse_params(pp, stream, &params, &variadic, &valid);
        if (st == QCC_OK && valid) {
            st = collect_replacement(stream, &repl);
        }
    } else {
        if (!is_line_end(&after)) {
            st = qcc_ptok_list_push(&repl, &after);
            if (st == QCC_OK) {
                st = collect_replacement(stream, &repl);
            }
        }
    }

    if (st == QCC_OK && valid) {
        validate_replacement(pp, func_like, variadic, &params, &repl);

        qcc_macro *m = build_macro(pp, &name, func_like, variadic, &params, &repl);
        if (m == NULL) {
            st = QCC_ERR_OUT_OF_MEMORY;
        } else {
            qcc_macro *existing = qcc_macro_lookup(pp->macros, name.spelling);
            if (existing != NULL && !qcc_macro_identical(existing, m)) {
                /* §6.10.3 ¶2: non-identical redefinition is a constraint
                   violation. We diagnose and take the new definition (matching
                   common practice), pointing at the previous one. */
                qcc_diag_emit(pp->diags, QCC_DIAG_WARNING, name.source,
                              name.offset, tok_span(&name), "'%s' redefined",
                              name.spelling);
                if (existing->def_source != NULL) {
                    qcc_diag_emit(pp->diags, QCC_DIAG_NOTE, existing->def_source,
                                  existing->def_offset, 1,
                                  "previous definition is here");
                }
            }
            st = qcc_macro_put(pp->macros, m);
        }
    }

    qcc_ptok_list_dispose(&repl);
    name_vec_free(&params);
    return st;
}

static qcc_status do_undef(qcc_pp *pp, qcc_pp_stream *stream)
{
    qcc_ptok name;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &name, &fe);
    if (st != QCC_OK) {
        return st;
    }
    if (name.kind != QCC_PP_TOKEN_IDENTIFIER) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name.source, name.offset,
                      tok_span(&name), "macro name must be an identifier");
        return is_line_end(&name) ? QCC_OK : skip_to_line_end(stream);
    }

    (void)qcc_macro_remove(pp->macros, name.spelling); /* No error if absent. */

    /* §6.10 ¶? : warn on tokens after the name (gcc-compatible). */
    qcc_ptok t;
    st = qcc_pp_stream_next(stream, &t, &fe);
    if (st != QCC_OK) {
        return st;
    }
    if (!is_line_end(&t)) {
        qcc_diag_emit(pp->diags, QCC_DIAG_WARNING, t.source, t.offset,
                      tok_span(&t), "extra tokens at end of #undef directive");
        return skip_to_line_end(stream);
    }
    return QCC_OK;
}

/*
 * Read the next token; if it is not the end of the line, optionally warn about
 * the extra tokens and then skip to the line end. Used by directives that take
 * no further tokens (#else, #endif, #ifdef name, …).
 */
static qcc_status expect_line_end(qcc_pp *pp, qcc_pp_stream *stream, int warn,
                                  const char *directive)
{
    qcc_ptok t;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &t, &fe);
    if (st != QCC_OK) {
        return st;
    }
    if (is_line_end(&t)) {
        return QCC_OK;
    }
    if (warn) {
        qcc_diag_emit(pp->diags, QCC_DIAG_WARNING, t.source, t.offset,
                      tok_span(&t), "extra tokens at end of #%s directive",
                      directive);
    }
    return skip_to_line_end(stream);
}

/* #if: evaluate the controlling expression (only when the enclosing region is
   emitting) and open a conditional (§6.10.1). */
static qcc_status do_if(qcc_pp *pp, qcc_pp_stream *stream)
{
    int        emitting = qcc_cond_emitting(pp->conds);
    int        cond     = 0;
    qcc_status st       = QCC_OK;

    if (emitting) {
        qcc_ptok_list line;
        qcc_ptok_list_init(&line);
        st = collect_replacement(stream, &line); /* Tokens up to the newline. */
        if (st == QCC_OK) {
            st = qcc_pp_eval_condition(pp, line.items, line.count, &cond);
        }
        qcc_ptok_list_dispose(&line);
    } else {
        st = skip_to_line_end(stream); /* Skipped group: do not evaluate. */
    }
    if (st != QCC_OK) {
        return st;
    }
    return qcc_cond_push(pp->conds, emitting, cond);
}

/* #ifdef / #ifndef: open a conditional on whether a macro is defined. */
static qcc_status do_ifdef(qcc_pp *pp, qcc_pp_stream *stream, int negate)
{
    int      emitting = qcc_cond_emitting(pp->conds);
    qcc_ptok name;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &name, &fe);
    if (st != QCC_OK) {
        return st;
    }

    int cond = 0;
    if (name.kind != QCC_PP_TOKEN_IDENTIFIER) {
        if (emitting) {
            qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name.source, name.offset,
                          tok_span(&name), "macro name must be an identifier");
        }
        if (!is_line_end(&name)) {
            st = skip_to_line_end(stream);
        }
    } else {
        int defined = (qcc_macro_lookup(pp->macros, name.spelling) != NULL);
        cond = negate ? !defined : defined;
        st = expect_line_end(pp, stream, emitting,
                             negate ? "ifndef" : "ifdef");
    }
    if (st != QCC_OK) {
        return st;
    }
    return qcc_cond_push(pp->conds, emitting, cond);
}

/* #elif: select this group if no earlier one was taken and the expression is
   nonzero (§6.10.1 ¶2). */
static qcc_status do_elif(qcc_pp *pp, qcc_pp_stream *stream)
{
    qcc_cond *frame = qcc_cond_top(pp->conds);
    if (frame == NULL) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, NULL, 0, 1, "#elif without #if");
        return skip_to_line_end(stream);
    }
    if (frame->seen_else) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, NULL, 0, 1, "#elif after #else");
        frame->active = 0;
        return skip_to_line_end(stream);
    }

    if (frame->outer_active && !frame->taken) {
        qcc_ptok_list line;
        qcc_ptok_list_init(&line);
        qcc_status st = collect_replacement(stream, &line);
        int cond = 0;
        if (st == QCC_OK) {
            st = qcc_pp_eval_condition(pp, line.items, line.count, &cond);
        }
        qcc_ptok_list_dispose(&line);
        if (st != QCC_OK) {
            return st;
        }
        frame->active = cond ? 1 : 0;
        if (frame->active) {
            frame->taken = 1;
        }
        return QCC_OK;
    }

    frame->active = 0; /* Outer inactive or already taken: skip without eval. */
    return skip_to_line_end(stream);
}

/* #else: select this group if none earlier was taken (§6.10.1 ¶2). */
static qcc_status do_else(qcc_pp *pp, qcc_pp_stream *stream)
{
    qcc_cond *frame = qcc_cond_top(pp->conds);
    if (frame == NULL) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, NULL, 0, 1, "#else without #if");
        return skip_to_line_end(stream);
    }
    if (frame->seen_else) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, NULL, 0, 1, "#else after #else");
        return skip_to_line_end(stream);
    }
    frame->seen_else = 1;
    if (frame->outer_active && !frame->taken) {
        frame->active = 1;
        frame->taken  = 1;
    } else {
        frame->active = 0;
    }
    return expect_line_end(pp, stream, frame->outer_active, "else");
}

/* #endif: close the innermost conditional (§6.10.1 ¶2). */
static qcc_status do_endif(qcc_pp *pp, qcc_pp_stream *stream)
{
    int had = qcc_cond_pop(pp->conds);
    if (!had) {
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, NULL, 0, 1, "#endif without #if");
    }
    return expect_line_end(pp, stream, had, "endif");
}

/* Public interface. */

qcc_status qcc_pp_directive(qcc_pp *pp, qcc_pp_stream *stream, const qcc_ptok *hash)
{
    (void)hash;
    if (pp == NULL || stream == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_ptok name;
    int      fe;
    qcc_status st = qcc_pp_stream_next(stream, &name, &fe);
    if (st != QCC_OK) {
        return st;
    }

    if (is_line_end(&name)) {
        return QCC_OK; /* Null directive (§6.10.7): '#' alone on a line. */
    }

    if (name.kind == QCC_PP_TOKEN_IDENTIFIER) {
        /* Conditional-inclusion directives are processed in both emitting and
           skipped regions, because the nesting must be tracked while skipping
           (§6.10.1 ¶6). */
        if (strcmp(name.spelling, "if") == 0) {
            return do_if(pp, stream);
        }
        if (strcmp(name.spelling, "ifdef") == 0) {
            return do_ifdef(pp, stream, 0);
        }
        if (strcmp(name.spelling, "ifndef") == 0) {
            return do_ifdef(pp, stream, 1);
        }
        if (strcmp(name.spelling, "elif") == 0) {
            return do_elif(pp, stream);
        }
        if (strcmp(name.spelling, "else") == 0) {
            return do_else(pp, stream);
        }
        if (strcmp(name.spelling, "endif") == 0) {
            return do_endif(pp, stream);
        }

        /* All other directives are inert inside a skipped group: only their
           presence is noted, never executed or diagnosed (§6.10.1 ¶6). */
        if (!qcc_cond_emitting(pp->conds)) {
            return skip_to_line_end(stream);
        }

        if (strcmp(name.spelling, "define") == 0) {
            return do_define(pp, stream);
        }
        if (strcmp(name.spelling, "undef") == 0) {
            return do_undef(pp, stream);
        }
        /* #include and #line/#error/#pragma land in the next step; diagnose for
           now so an unknown directive is never silently accepted. */
        qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name.source, name.offset,
                      tok_span(&name),
                      "unsupported preprocessing directive '#%s'", name.spelling);
        return skip_to_line_end(stream);
    }

    if (!qcc_cond_emitting(pp->conds)) {
        return skip_to_line_end(stream);
    }
    qcc_diag_emit(pp->diags, QCC_DIAG_ERROR, name.source, name.offset,
                  tok_span(&name), "invalid preprocessing directive");
    return skip_to_line_end(stream);
}
