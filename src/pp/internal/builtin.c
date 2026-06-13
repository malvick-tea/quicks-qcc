/*
 * qcc — preprocessor internals: predefined macros (implementation).
 *
 * See builtin.h. __LINE__/__FILE__ are tagged builtins (pp/internal/expand
 * computes them per use). The fixed-value macros are installed as ordinary
 * object-like macros with a one-token replacement built at startup. __DATE__
 * and __TIME__ take the host clock at install time (a seed-CRT dependency,
 * ADR-0009).
 *
 * Spec anchors: §6.10.8.1 (__DATE__, __FILE__, __LINE__, __STDC__,
 * __STDC_HOSTED__, __TIME__), §6.10.8.1 (__STDC_VERSION__ == 201112L for C11).
 */
#include "pp/internal/builtin.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pp/internal/macro.h"

/* Build a one-token object-like macro `name` -> a single token of `kind` with
   the given interned-able `spelling`, and install it. */
static qcc_status install_object(qcc_pp *pp, const char *name,
                                 qcc_pp_token_kind kind, const char *spelling)
{
    qcc_macro *m =
        (qcc_macro *)qcc_arena_alloc(&pp->arena, sizeof(*m), _Alignof(qcc_macro));
    if (m == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    memset(m, 0, sizeof(*m));
    m->name = qcc_pp_intern(pp, name, strlen(name));
    if (m->name == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    qcc_ptok *r =
        (qcc_ptok *)qcc_arena_alloc(&pp->arena, sizeof(*r), _Alignof(qcc_ptok));
    if (r == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    memset(r, 0, sizeof(*r));
    r->kind         = kind;
    r->spelling     = qcc_pp_intern(pp, spelling, strlen(spelling));
    r->spelling_len = strlen(spelling);
    if (r->spelling == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    m->replacement       = r;
    m->replacement_count = 1;
    return qcc_macro_put(pp->macros, m);
}

/* Install a position-dependent builtin (__LINE__ / __FILE__) with no stored
   replacement; pp/internal/expand computes its value at each use. */
static qcc_status install_builtin(qcc_pp *pp, const char *name,
                                  qcc_macro_builtin which)
{
    qcc_macro *m =
        (qcc_macro *)qcc_arena_alloc(&pp->arena, sizeof(*m), _Alignof(qcc_macro));
    if (m == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    memset(m, 0, sizeof(*m));
    m->name = qcc_pp_intern(pp, name, strlen(name));
    if (m->name == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    m->builtin = which;
    return qcc_macro_put(pp->macros, m);
}

qcc_status qcc_pp_install_builtins(qcc_pp *pp)
{
    if (pp == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    qcc_status st;
    if ((st = install_builtin(pp, "__LINE__", QCC_MACRO_BUILTIN_LINE)) != QCC_OK ||
        (st = install_builtin(pp, "__FILE__", QCC_MACRO_BUILTIN_FILE)) != QCC_OK) {
        return st;
    }

    /* Fixed-value macros (§6.10.8.1). qcc is a freestanding compiler
       (ADR-0013), so __STDC_HOSTED__ is 0; __STDC_VERSION__ is the C11 value. */
    if ((st = install_object(pp, "__STDC__", QCC_PP_TOKEN_PP_NUMBER, "1")) != QCC_OK ||
        (st = install_object(pp, "__STDC_VERSION__", QCC_PP_TOKEN_PP_NUMBER,
                             "201112L")) != QCC_OK ||
        (st = install_object(pp, "__STDC_HOSTED__", QCC_PP_TOKEN_PP_NUMBER,
                             "0")) != QCC_OK) {
        return st;
    }

    /* __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss") from the host clock. */
    static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May",
                                             "Jun", "Jul", "Aug", "Sep", "Oct",
                                             "Nov", "Dec" };
    char        date_buf[16];
    char        time_buf[16];
    time_t      now = time(NULL);
    struct tm  *lt  = localtime(&now);
    if (lt != NULL) {
        int mon = (lt->tm_mon >= 0 && lt->tm_mon < 12) ? lt->tm_mon : 0;
        /* §6.10.8.1: the day is space-padded to two characters. */
        snprintf(date_buf, sizeof(date_buf), "\"%s %2d %d\"", months[mon],
                 lt->tm_mday, 1900 + lt->tm_year);
        snprintf(time_buf, sizeof(time_buf), "\"%02d:%02d:%02d\"", lt->tm_hour,
                 lt->tm_min, lt->tm_sec);
    } else {
        /* Defensive fallback if the clock is unavailable. */
        snprintf(date_buf, sizeof(date_buf), "\"Jan  1 1970\"");
        snprintf(time_buf, sizeof(time_buf), "\"00:00:00\"");
    }
    if ((st = install_object(pp, "__DATE__", QCC_PP_TOKEN_STRING_LIT,
                             date_buf)) != QCC_OK ||
        (st = install_object(pp, "__TIME__", QCC_PP_TOKEN_STRING_LIT,
                             time_buf)) != QCC_OK) {
        return st;
    }

    return QCC_OK;
}
