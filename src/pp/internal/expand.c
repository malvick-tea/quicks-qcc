/*
 * qcc — preprocessor internals: macro expansion (implementation).
 *
 * See expand.h for the contract and the §6.10.3.4 rationale. This step
 * implements object-like expansion; function-like expansion (argument
 * collection, parameter substitution, #, ##) builds on the same hide-set
 * machinery and is added next.
 */
#include "pp/internal/expand.h"

#include "pp/internal/hideset.h"

/*
 * Object-like expansion (§6.10.3 ¶9). The new hide set is HS(name) ∪ {name}:
 * every replacement token inherits it, so the macro's own name is "painted" and
 * will not re-expand during the rescan (§6.10.3.4 ¶2). The substituted list is
 * copied into the arena (the stream borrows it until exhausted) with each
 * token's hide set unioned with the new set, line-start cleared (a '#' from a
 * replacement is never a directive), and the first token taking the call site's
 * leading-space so output spacing is preserved.
 */
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
        return QCC_OK; /* Empty replacement: the name simply disappears. */
    }

    qcc_ptok *copy =
        (qcc_ptok *)qcc_arena_alloc(&pp->arena, n * sizeof(*copy), _Alignof(qcc_ptok));
    if (copy == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < n; ++i) {
        copy[i] = macro->replacement[i];

        const qcc_hideset *unioned = NULL;
        st = qcc_hideset_union(&pp->arena, copy[i].hideset, new_hs, &unioned);
        if (st != QCC_OK) {
            return st;
        }
        copy[i].hideset       = unioned;
        copy[i].at_line_start = 0; /* Replacement tokens never start a directive.*/
    }
    copy[0].leading_space = name->leading_space; /* Preserve call-site spacing.   */

    return qcc_pp_stream_push_tokens(stream, copy, n);
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
        /* A function-like macro name expands only when followed by '(' — the
           argument-collection path lands in the next step. Until then it is
           left as an ordinary identifier (which is also the correct behavior
           when no '(' follows, §6.10.3 ¶10). */
        return QCC_OK;
    }

    qcc_status st = expand_object(pp, stream, name, macro);
    if (st != QCC_OK) {
        return st;
    }
    *out_expanded = 1;
    return QCC_OK;
}
