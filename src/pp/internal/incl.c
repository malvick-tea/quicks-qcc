/*
 * qcc — preprocessor internals: #include resolution and source ownership
 *        (implementation).
 *
 * See incl.h for the contract and ADR-0015 for the policy. The search order is
 * the de-facto standard (GCC/Clang): the quote form looks in the includer's own
 * directory, then the quote dirs, then the angle dirs (§6.10.2 ¶3's "reprocess
 * as <...>" fallback); the angle form looks only in the angle dirs (¶2). An
 * absolute header name bypasses the search. Every loaded file's qcc_source is
 * heap-owned here and disposed in bulk at teardown, because qcc_source buffers
 * come from the seed CRT allocator (ADR-0009), not the arena.
 */
#include "pp/internal/incl.h"

#include <stdlib.h>
#include <string.h>

/* Private helpers. */

/* A path separator on the seed host: POSIX '/' or Windows '\\' (ADR-0015). */
static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* An ASCII letter, for the Windows "X:" drive-qualified absolute prefix. */
static int is_drive_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/*
 * Is `name` an absolute path that must be opened directly with no search? True
 * for a leading separator ("/x", "\\x") and for a drive-qualified prefix on the
 * host ("C:..."). §6.10.2 leaves this implementation-defined; we treat the
 * platform's own notion of absolute as absolute.
 */
static int is_absolute(const char *name, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (is_sep(name[0])) {
        return 1;
    }
    return len >= 2 && is_drive_letter(name[0]) && name[1] == ':';
}

/*
 * Join a directory and a header name into a NUL-terminated path in the arena. A
 * NULL/empty `dir` yields the name alone (searched relative to the current
 * directory); a separator is inserted only when `dir` does not already end with
 * one. Returns NULL on OOM.
 */
static char *build_path(qcc_incl *incl, const char *dir, const char *name,
                        size_t name_len)
{
    size_t dlen     = (dir != NULL) ? strlen(dir) : 0;
    int    need_sep = (dlen > 0 && !is_sep(dir[dlen - 1])) ? 1 : 0;
    size_t total    = dlen + (size_t)need_sep + name_len + 1; /* +1 for NUL. */

    char *path = (char *)qcc_arena_alloc(incl->arena, total, 1);
    if (path == NULL) {
        return NULL;
    }
    size_t k = 0;
    if (dlen > 0) {
        memcpy(path + k, dir, dlen);
        k += dlen;
        if (need_sep) {
            path[k++] = '/';
        }
    }
    memcpy(path + k, name, name_len);
    k += name_len;
    path[k] = '\0';
    return path;
}

/* Append a source pointer to the owned pool, growing geometrically. */
static qcc_status own_push(qcc_incl *incl, qcc_source *src)
{
    if (incl->owned_count == incl->owned_cap) {
        size_t       ncap  = (incl->owned_cap == 0) ? 8u : incl->owned_cap * 2u;
        qcc_source **grown =
            (qcc_source **)realloc(incl->owned, ncap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        incl->owned     = grown;
        incl->owned_cap = ncap;
    }
    incl->owned[incl->owned_count++] = src;
    return QCC_OK;
}

/* Append an arena-copied directory to one of the search vectors. */
static qcc_status dir_push(qcc_incl *incl, const char ***vec, size_t *count,
                           size_t *cap, const char *dir)
{
    const char *copy = qcc_arena_strdup(incl->arena, dir);
    if (copy == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    if (*count == *cap) {
        size_t       ncap  = (*cap == 0) ? 4u : *cap * 2u;
        const char **grown =
            (const char **)realloc((void *)*vec, ncap * sizeof(*grown));
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        *vec = grown;
        *cap = ncap;
    }
    (*vec)[(*count)++] = copy;
    return QCC_OK;
}

/*
 * Try to load <dir>/<name> (or just <name> when dir is NULL/empty). On success
 * the source is added to the owned pool and *out is set. Returns QCC_OK,
 * QCC_ERR_IO (this place does not have the file — try the next), or
 * QCC_ERR_OUT_OF_MEMORY.
 */
static qcc_status try_dir(qcc_incl *incl, const char *dir, const char *name,
                          size_t name_len, const qcc_source **out)
{
    char *path = build_path(incl, dir, name, name_len);
    if (path == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    qcc_source *src = (qcc_source *)malloc(sizeof(*src));
    if (src == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    qcc_status st = qcc_source_load_file(path, src);
    if (st != QCC_OK) {
        free(src);
        return st; /* QCC_ERR_IO (not here) or a hard fault. */
    }
    st = own_push(incl, src);
    if (st != QCC_OK) {
        qcc_source_dispose(src);
        free(src);
        return st;
    }
    *out = src;
    return QCC_OK;
}

/* Public interface. */

void qcc_incl_init(qcc_incl *incl, qcc_arena *arena)
{
    if (incl == NULL) {
        return;
    }
    memset(incl, 0, sizeof(*incl));
    incl->arena = arena;
}

void qcc_incl_dispose(qcc_incl *incl)
{
    if (incl == NULL) {
        return;
    }
    for (size_t i = 0; i < incl->owned_count; ++i) {
        qcc_source_dispose(incl->owned[i]);
        free(incl->owned[i]);
    }
    free(incl->owned);
    free((void *)incl->quote_dirs);
    free((void *)incl->angle_dirs);
    memset(incl, 0, sizeof(*incl));
}

qcc_status qcc_incl_add_angle_dir(qcc_incl *incl, const char *dir)
{
    if (incl == NULL || dir == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    return dir_push(incl, &incl->angle_dirs, &incl->angle_count,
                    &incl->angle_cap, dir);
}

qcc_status qcc_incl_add_quote_dir(qcc_incl *incl, const char *dir)
{
    if (incl == NULL || dir == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    return dir_push(incl, &incl->quote_dirs, &incl->quote_count,
                    &incl->quote_cap, dir);
}

qcc_status qcc_incl_open(qcc_incl *incl, const char *name, size_t name_len,
                         int is_angle, const char *includer_dir,
                         const qcc_source **out_source)
{
    if (incl == NULL || name == NULL || out_source == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    *out_source = NULL;
    if (name_len == 0) {
        return QCC_ERR_IO; /* An empty header name resolves to nothing. */
    }

    /* An absolute path is opened directly; the search list does not apply. */
    if (is_absolute(name, name_len)) {
        qcc_status st = try_dir(incl, NULL, name, name_len, out_source);
        return (st == QCC_ERR_IO) ? QCC_ERR_IO : st;
    }

    /* The quote form looks in the includer's directory, then the quote dirs,
       before falling through to the angle dirs (§6.10.2 ¶3). */
    if (!is_angle) {
        qcc_status st = try_dir(incl, (includer_dir != NULL) ? includer_dir : "",
                                name, name_len, out_source);
        if (st != QCC_ERR_IO) {
            return st; /* QCC_OK or a hard fault. */
        }
        for (size_t i = 0; i < incl->quote_count; ++i) {
            st = try_dir(incl, incl->quote_dirs[i], name, name_len, out_source);
            if (st != QCC_ERR_IO) {
                return st;
            }
        }
    }

    /* The angle form (and the quote fallback) searches the angle dirs (¶2). */
    for (size_t i = 0; i < incl->angle_count; ++i) {
        qcc_status st = try_dir(incl, incl->angle_dirs[i], name, name_len,
                                out_source);
        if (st != QCC_ERR_IO) {
            return st;
        }
    }
    return QCC_ERR_IO;
}

const char *qcc_incl_dirname(qcc_incl *incl, const char *path)
{
    if (incl == NULL || path == NULL) {
        return "";
    }
    size_t len  = strlen(path);
    size_t last = (size_t)-1;
    for (size_t i = 0; i < len; ++i) {
        if (is_sep(path[i])) {
            last = i;
        }
    }
    if (last == (size_t)-1) {
        return ""; /* No directory component: the current directory. */
    }
    if (last == 0) {
        return "/"; /* "/foo" lives in the root directory. */
    }

    char *dir = (char *)qcc_arena_alloc(incl->arena, last + 1, 1);
    if (dir == NULL) {
        return ""; /* Degrade to the current directory rather than fault. */
    }
    memcpy(dir, path, last);
    dir[last] = '\0';
    return dir;
}
