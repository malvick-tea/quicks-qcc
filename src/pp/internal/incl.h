/*
 * qcc — preprocessor internals: #include resolution and source ownership
 *        (ISO C11 §6.10.2, §6.4.7)
 *
 * Responsibility
 * Turn a header reference — a name plus its form (angle `<...>` or quote
 * `"..."`) and the directory of the file that wrote the #include — into a
 * loaded qcc_source, and own that source for the rest of the preprocessing run.
 *
 * Two concerns live here because they are inseparable:
 *
 *   1. Search policy (§6.10.2 ¶2-4). The angle form searches the *angle*
 *      directories; the quote form searches the includer's own directory first,
 *      then the *quote* directories, then falls back to the angle directories
 *      (the ¶3 "reprocess as if <...>"). An absolute path bypasses the search.
 *      Design and the exact order are recorded in ADR-0015.
 *   2. Source lifetime. The translation unit's root source is owned by the
 *      caller of qcc_pp_run; an *included* source is created during the run and
 *      must outlive every qcc_ptok lexed from it and every diagnostic that
 *      points into it (which are printed after the run). So qcc_incl keeps a
 *      pool of every source it loads and disposes each at teardown.
 *
 * Path handling is host-shaped (the seed CRT, ADR-0009): both '/' and '\\'
 * separate components and a drive-qualified prefix (e.g. "C:") is absolute, so
 * the Windows MSYS host and POSIX hosts both work. The interface is unchanged
 * when qlibc replaces the path primitives.
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_INCL_H
#define QCC_PP_INTERNAL_INCL_H

#include <stddef.h>

#include "arena/arena.h"
#include "source/source.h"
#include "status/status.h"

/*
 * Maximum number of nested file (#include) inputs. ISO C11 §5.2.4.1 only
 * guarantees 15 levels of nesting as a *minimum* translation limit and sets no
 * maximum, so an implementation must cap nesting itself to stay terminating on a
 * guard-less include cycle. 200 is far above any real nesting and any sane code,
 * so it never rejects valid input while still bounding a cycle (ADR-0015).
 */
#define QCC_INCL_MAX_DEPTH ((size_t)200u)

/*
 * The include resolver. Borrows an arena (for the interned directory strings and
 * the candidate path scratch); owns the heap vectors below and every qcc_source
 * it loads. Treat the fields as private; use the functions.
 */
typedef struct qcc_incl {
    qcc_arena   *arena;        /* Borrowed: backs dir strings + path scratch.    */

    const char **quote_dirs;   /* Searched for "..." (after the includer dir).   */
    size_t       quote_count;
    size_t       quote_cap;

    const char **angle_dirs;   /* Searched for <...> (and the "..." fallback).   */
    size_t       angle_count;
    size_t       angle_cap;

    qcc_source **owned;        /* Every loaded source; each disposed on teardown.*/
    size_t       owned_count;
    size_t       owned_cap;
} qcc_incl;

/* Initialize an empty resolver bound to `arena` (must outlive the resolver and
   be non-NULL). No directories, no loaded sources. Always succeeds. */
void qcc_incl_init(qcc_incl *incl, qcc_arena *arena);

/*
 * Dispose every source the resolver loaded and free its heap vectors, then zero
 * it. The arena and its directory strings are NOT freed here (they die with the
 * arena). Idempotent and NULL-safe. After this, any qcc_source the resolver
 * handed out is invalid.
 */
void qcc_incl_dispose(qcc_incl *incl);

/*
 * Append a search directory. `dir` is copied into the arena. An empty string
 * means "the current directory" and is kept as-is (resolution treats it as no
 * prefix). Returns QCC_OK, or QCC_ERR_INVALID_ARGUMENT / QCC_ERR_OUT_OF_MEMORY.
 *
 *   add_angle_dir : searched by both <...> and the "..." fallback (a -I dir).
 *   add_quote_dir : searched by "..." only, before the angle dirs (a -iquote dir).
 */
qcc_status qcc_incl_add_angle_dir(qcc_incl *incl, const char *dir);
qcc_status qcc_incl_add_quote_dir(qcc_incl *incl, const char *dir);

/*
 * Resolve and load a header.
 *
 *   name/name_len : the header text WITHOUT its delimiters (e.g. "sys/types.h").
 *                   Need not be NUL-terminated; name_len is authoritative.
 *   is_angle      : 1 for the <...> form, 0 for the "..." form (§6.10.2 ¶2/¶3).
 *   includer_dir  : directory of the file containing the #include, used as the
 *                   first place searched for the quote form. NULL or "" means
 *                   the current directory. Ignored for the angle form.
 *   out_source    : on success receives a source OWNED by `incl` — do not
 *                   dispose it; it stays valid until qcc_incl_dispose.
 *
 * Returns:
 *   QCC_OK                 found and loaded; *out_source set.
 *   QCC_ERR_IO             not found in any searched place (caller diagnoses).
 *   QCC_ERR_OUT_OF_MEMORY  allocation failure.
 *   QCC_ERR_INVALID_ARGUMENT on a NULL/!len contract violation.
 */
qcc_status qcc_incl_open(qcc_incl *incl, const char *name, size_t name_len,
                         int is_angle, const char *includer_dir,
                         const qcc_source **out_source);

/*
 * Return the directory part of `path` (everything up to and including the last
 * path separator, with the separator removed), interned in the arena, or "" if
 * `path` has no separator. Used by the directive to derive the includer
 * directory from the current source's name. NULL `path` yields "".
 */
const char *qcc_incl_dirname(qcc_incl *incl, const char *path);

#endif /* QCC_PP_INTERNAL_INCL_H */
