/*
 * qcc — the preprocessor: the public driver (realizes pp.h).
 *
 * Scope at this stage (ADR-0014, built front-to-back): the preprocessor owns an
 * arena and interner and *materializes* the lexer's span-backed qcc_pp_token
 * stream into qcc_ptok values — interning each spelling, recording provenance,
 * and starting every token with an empty hide set. Directive execution
 * (§6.10.1-.6) and macro expansion (§6.10.3) land in the internal submodules
 * (directive/expand/cond/ceval/stream) and are wired through here as they
 * arrive; until then qcc_pp_run is a faithful phase-1-3 pass-through expressed
 * in the phase-4 token type, which is exactly what later stages consume.
 *
 * The materialization step is the single boundary between the two token
 * vocabularies: below it, span-backed qcc_pp_token from the lexer; above it,
 * materialized qcc_ptok. Keeping it in one place is why no other pp code needs
 * to know how spellings are recovered (splice-free) from source bytes.
 */
#include "pp/pp.h"

#include <stdlib.h>

#include "lexer/lexer.h"

/*
 * Initial scratch buffer for recovering a token's splice-free spelling before
 * interning it. A token's logical spelling is never longer than its physical
 * span, so the buffer is grown to the largest span seen and reused; most tokens
 * are a handful of bytes, so it rarely grows.
 */
#define QCC_PP_SCRATCH_INITIAL ((size_t)128u)

/* Private helpers. */

/*
 * Materialize one lexer token into *out: recover its splice-free spelling into
 * *scratch (grown if needed), intern it, and copy kind/punct/provenance/flags.
 * The hide set starts empty (NULL). Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY.
 */
static qcc_status materialize(qcc_pp *pp, const qcc_source *src,
                              const qcc_pp_token *in, char **scratch,
                              size_t *scratch_cap, qcc_ptok *out)
{
    if (in->length > *scratch_cap) {
        size_t new_cap = in->length;
        char  *grown   = (char *)realloc(*scratch, new_cap);
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        *scratch     = grown;
        *scratch_cap = new_cap;
    }

    /* qcc_lexer_token_spelling copies the logical (splice-free) bytes and
       returns their count, which is <= in->length <= *scratch_cap, so the copy
       is never truncated. */
    size_t      len      = qcc_lexer_token_spelling(src, in, *scratch, *scratch_cap);
    const char *interned = qcc_intern_bytes(&pp->interner, *scratch, len);
    if (interned == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    out->kind          = in->kind;
    out->punct         = in->punct;
    out->spelling      = interned;
    out->spelling_len  = len;
    out->source        = src;
    out->offset        = in->offset;
    out->line          = in->line;
    out->column        = in->column;
    out->leading_space = in->leading_space;
    out->at_line_start = in->at_line_start;
    out->hideset       = NULL;
    return QCC_OK;
}

/* Public interface. */

qcc_status qcc_pp_init(qcc_pp *pp, qcc_diag_sink *diags)
{
    if (pp == NULL || diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_arena_init(&pp->arena, 0);
    pp->diags = diags;

    qcc_status st = qcc_intern_init(&pp->interner, &pp->arena);
    if (st != QCC_OK) {
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

qcc_status qcc_pp_run(qcc_pp *pp, const qcc_source *source, qcc_ptok_list *out)
{
    if (pp == NULL || source == NULL || out == NULL || pp->diags == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_lexer lexer;
    qcc_lexer_init(&lexer, source, pp->diags);

    char  *scratch     = (char *)malloc(QCC_PP_SCRATCH_INITIAL);
    size_t scratch_cap = QCC_PP_SCRATCH_INITIAL;
    if (scratch == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    qcc_status st = QCC_OK;
    for (;;) {
        qcc_pp_token raw;
        st = qcc_lexer_next(&lexer, &raw);
        if (st != QCC_OK) {
            break;
        }

        qcc_ptok tok;
        st = materialize(pp, source, &raw, &scratch, &scratch_cap, &tok);
        if (st != QCC_OK) {
            break;
        }

        st = qcc_ptok_list_push(out, &tok);
        if (st != QCC_OK) {
            break;
        }

        if (raw.kind == QCC_PP_TOKEN_EOF) {
            break;
        }
    }

    free(scratch);
    return st;
}
