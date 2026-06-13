/*
 * qcc — the preprocessor: the public driver (realizes pp.h).
 *
 * The driver is the line-oriented loop of translation phase 4 (ISO C11 §6.10).
 * It pulls tokens from the input stream (pp/internal/stream, which owns the
 * lexer→qcc_ptok materialization and the include/rescan stack), and for each
 * token decides:
 *
 *   - a '#' that begins a file line (not from a macro expansion) introduces a
 *     directive (§6.10 ¶2) -> hand the rest of the line to pp/internal/directive;
 *   - an identifier that names a currently-defined macro and is not blocked by
 *     its own hide set is expanded (§6.10.3) via pp/internal/expand, which
 *     pushes the substituted replacement back onto the stream for rescanning;
 *   - everything else is emitted to the output token list.
 *
 * Newlines are consumed but not emitted: the phase-4 output `convert` consumes
 * has no newline tokens (line structure for a future -E rendering is recovered
 * from token provenance). The stream's final EOF terminates the output.
 *
 * The preprocessor owns an arena, an interner, and the macro table; together
 * they back every qcc_ptok it produces, so the output is valid only until
 * qcc_pp_dispose (pp.h).
 */
#include "pp/pp.h"

#include "pp/internal/builtin.h"
#include "pp/internal/directive.h"
#include "pp/internal/expand.h"
#include "pp/internal/hideset.h"
#include "pp/internal/macro.h"
#include "pp/internal/stream.h"

/* Public interface. */

qcc_status qcc_pp_init(qcc_pp *pp, qcc_diag_sink *diags)
{
    if (pp == NULL || diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&pp->arena, 0);
    pp->diags  = diags;
    pp->macros = NULL;

    qcc_status st = qcc_intern_init(&pp->interner, &pp->arena);
    if (st != QCC_OK) {
        qcc_arena_dispose(&pp->arena);
        return st;
    }

    /* The macro-table struct lives in the arena (freed with it); only its
       bucket array is separately heap-owned and released by table_dispose. */
    pp->macros = (qcc_macro_table *)qcc_arena_alloc(
        &pp->arena, sizeof(*pp->macros), _Alignof(qcc_macro_table));
    if (pp->macros == NULL) {
        qcc_intern_dispose(&pp->interner);
        qcc_arena_dispose(&pp->arena);
        return QCC_ERR_OUT_OF_MEMORY;
    }
    st = qcc_macro_table_init(pp->macros);
    if (st != QCC_OK) {
        pp->macros = NULL;
        qcc_intern_dispose(&pp->interner);
        qcc_arena_dispose(&pp->arena);
        return st;
    }

    /* Predefined macros (§6.10.8) are in scope from the first line. */
    st = qcc_pp_install_builtins(pp);
    if (st != QCC_OK) {
        qcc_macro_table_dispose(pp->macros);
        pp->macros = NULL;
        qcc_intern_dispose(&pp->interner);
        qcc_arena_dispose(&pp->arena);
        return st;
    }
    return QCC_OK;
}

void qcc_pp_dispose(qcc_pp *pp)
{
    if (pp == NULL) {
        return;
    }
    if (pp->macros != NULL) {
        qcc_macro_table_dispose(pp->macros); /* Frees the bucket array. */
        pp->macros = NULL;
    }
    qcc_intern_dispose(&pp->interner);
    qcc_arena_dispose(&pp->arena); /* Frees every spelling, macro, hide set. */
    pp->diags = NULL;
}

const char *qcc_pp_intern(qcc_pp *pp, const char *bytes, size_t length)
{
    if (pp == NULL) {
        return NULL;
    }
    return qcc_intern_bytes(&pp->interner, bytes, length);
}

/*
 * Is this token a directive introducer? Only a '#' punctuator that starts a
 * logical line and came directly from a file (not a macro expansion) — exactly
 * §6.10 ¶2 plus §6.10.3.4 ¶3 (a '#' produced by macro expansion is not a
 * directive).
 */
static int is_directive_intro(const qcc_ptok *tok, int from_expansion)
{
    return !from_expansion && tok->at_line_start &&
           tok->kind == QCC_PP_TOKEN_PUNCT && tok->punct == QCC_PUNCT_HASH;
}

qcc_status qcc_pp_run(qcc_pp *pp, const qcc_source *source, qcc_ptok_list *out)
{
    if (pp == NULL || source == NULL || out == NULL || pp->diags == NULL ||
        pp->macros == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_pp_stream stream;
    qcc_status    st = qcc_pp_stream_init(&stream, pp, source);
    if (st != QCC_OK) {
        return st;
    }

    for (;;) {
        qcc_ptok tok;
        int      from_expansion = 0;
        st = qcc_pp_stream_next(&stream, &tok, &from_expansion);
        if (st != QCC_OK) {
            break;
        }

        if (tok.kind == QCC_PP_TOKEN_EOF) {
            st = qcc_ptok_list_push(out, &tok); /* Terminate the output. */
            break;
        }
        if (tok.kind == QCC_PP_TOKEN_NEWLINE) {
            continue; /* Line structure is not part of the phase-4 token output. */
        }

        if (is_directive_intro(&tok, from_expansion)) {
            st = qcc_pp_directive(pp, &stream, &tok);
            if (st != QCC_OK) {
                break;
            }
            continue;
        }

        if (tok.kind == QCC_PP_TOKEN_IDENTIFIER) {
            qcc_macro *macro = qcc_macro_lookup(pp->macros, tok.spelling);
            if (macro != NULL && !qcc_hideset_contains(tok.hideset, tok.spelling)) {
                int expanded = 0;
                st = qcc_pp_expand(pp, &stream, &tok, macro, &expanded);
                if (st != QCC_OK) {
                    break;
                }
                if (expanded) {
                    continue; /* Rescan the pushed replacement. */
                }
            }
        }

        st = qcc_ptok_list_push(out, &tok);
        if (st != QCC_OK) {
            break;
        }
    }

    qcc_pp_stream_dispose(&stream);
    return st;
}
