/*
 * qcc — command-line entry point.
 *
 * What works today
 *   The front end runs through translation phase 4 (the preprocessor; ADR-0013
 *   pipeline, ADR-0014 design). `qcc -E <in.c>` preprocesses a translation unit
 *   and writes the resulting text — every §6.10 directive, macro expansion, and
 *   `#include` along the search path. The later stages (convert, parse, …) land
 *   front to back; without `-E`, qcc still reports that full translation is not
 *   implemented yet rather than pretending to compile.
 *
 * Exit codes (stable contract, shared shape with qas/qld)
 *   0  success
 *   1  the input produced errors, or an I/O error occurred
 *   2  usage error (bad/missing arguments)
 *
 * This file is the only place that talks to argv and stdout/stderr for the tool;
 * the library modules (qcc_core) stay free of I/O policy so they remain testable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag/diag.h"
#include "pp/pp.h"
#include "pp/render.h"
#include "source/source.h"
#include "status/status.h"

/* Exit codes, named so call sites read clearly (no magic numbers). */
enum {
    QCC_EXIT_OK    = 0,
    QCC_EXIT_ERROR = 1,
    QCC_EXIT_USAGE = 2
};

/* A growable list of borrowed directory strings (pointers into argv). */
typedef struct dirlist {
    const char **items;
    size_t       count;
    size_t       cap;
} dirlist;

static int dirlist_push(dirlist *l, const char *s)
{
    if (l->count == l->cap) {
        size_t       ncap  = (l->cap == 0) ? 4u : l->cap * 2u;
        const char **grown = (const char **)realloc((void *)l->items,
                                                    ncap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        l->items = grown;
        l->cap   = ncap;
    }
    l->items[l->count++] = s;
    return 1;
}

static void dirlist_free(dirlist *l)
{
    free((void *)l->items);
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "qcc — the Quicks C compiler\n"
            "\n"
            "Usage:\n"
            "  %s -E [-I <dir>]... [-iquote <dir>]... [-o <out>] <in.c>\n"
            "                             Preprocess <in.c> (translation phase 4)\n"
            "  %s [-o <out.s>] <in.c>     Compile <in.c> to x86-64 assembly (Intel\n"
            "                             syntax, for qas) — not yet implemented\n"
            "  %s --help                  Show this help\n"
            "  %s --version               Show version information\n"
            "\n"
            "Options:\n"
            "  -E            Preprocess only; write the result to stdout (or -o).\n"
            "  -I <dir>      Add <dir> to the angle (<...>) include search path.\n"
            "  -iquote <dir> Add <dir> to the quote (\"...\") include search path.\n"
            "  -o <out>      Write output to <out> instead of stdout / <in>.s.\n",
            prog, prog, prog, prog);
}

/* Render the preprocessed token stream to `output_path` (NULL => stdout).
   Returns 1 on success, 0 on a render or I/O failure (message already printed). */
static int write_rendered(const qcc_ptok_list *toks, const char *output_path)
{
    char      *text = NULL;
    size_t     len  = 0;
    qcc_status st   = qcc_pp_render(toks, &text, &len);
    if (st != QCC_OK) {
        fprintf(stderr, "qcc: error: %s\n", qcc_status_str(st));
        return 0;
    }

    FILE *outf      = stdout;
    int   close_out = 0;
    if (output_path != NULL) {
        outf = fopen(output_path, "wb");
        if (outf == NULL) {
            fprintf(stderr, "qcc: error: cannot open '%s' for writing\n",
                    output_path);
            free(text);
            return 0;
        }
        close_out = 1;
    }

    int ok = 1;
    if (len > 0 && fwrite(text, 1, len, outf) != len) {
        fprintf(stderr, "qcc: error: writing output failed\n");
        ok = 0;
    }
    if (close_out) {
        if (fclose(outf) != 0) {
            ok = 0;
        }
    } else {
        fflush(outf);
    }
    free(text);
    return ok;
}

/* The `qcc -E` path: preprocess `input_path` and write the rendered text. */
static int run_preprocess(const char *input_path, const char *output_path,
                          const dirlist *angle, const dirlist *quote)
{
    qcc_source source;
    qcc_status st = qcc_source_load_file(input_path, &source);
    if (st != QCC_OK) {
        fprintf(stderr, "qcc: error: cannot read '%s': %s\n", input_path,
                qcc_status_str(st));
        return QCC_EXIT_ERROR;
    }

    qcc_diag_sink diags;
    qcc_diag_sink_init(&diags);

    qcc_pp pp;
    st = qcc_pp_init(&pp, &diags);
    if (st != QCC_OK) {
        fprintf(stderr, "qcc: error: %s\n", qcc_status_str(st));
        qcc_diag_sink_dispose(&diags);
        qcc_source_dispose(&source);
        return QCC_EXIT_ERROR;
    }

    int cfg_ok = 1;
    for (size_t i = 0; i < angle->count && cfg_ok; ++i) {
        cfg_ok = (qcc_pp_add_include_dir(&pp, angle->items[i]) == QCC_OK);
    }
    for (size_t i = 0; i < quote->count && cfg_ok; ++i) {
        cfg_ok = (qcc_pp_add_quote_include_dir(&pp, quote->items[i]) == QCC_OK);
    }

    int exit_code = QCC_EXIT_OK;
    if (!cfg_ok) {
        fprintf(stderr, "qcc: error: out of memory configuring include path\n");
        exit_code = QCC_EXIT_ERROR;
    } else {
        qcc_ptok_list out;
        qcc_ptok_list_init(&out);
        st = qcc_pp_run(&pp, &source, &out);
        if (st != QCC_OK) {
            fprintf(stderr, "qcc: error: preprocessing failed: %s\n",
                    qcc_status_str(st));
            exit_code = QCC_EXIT_ERROR;
        } else if (!write_rendered(&out, output_path)) {
            exit_code = QCC_EXIT_ERROR;
        }
        qcc_ptok_list_dispose(&out);
    }

    /* Report every diagnostic; a single error makes the run a failure. */
    qcc_diag_sink_print(&diags, stderr);
    if (exit_code == QCC_EXIT_OK &&
        qcc_diag_severity_count(&diags, QCC_DIAG_ERROR) > 0) {
        exit_code = QCC_EXIT_ERROR;
    }

    qcc_pp_dispose(&pp);
    qcc_diag_sink_dispose(&diags);
    qcc_source_dispose(&source);
    return exit_code;
}

/* The full-compile path: validate the input exists, then report honestly that
   translation past phase 4 is not implemented yet. */
static int report_not_implemented(const char *input_path, const char *output_path)
{
    qcc_source source;
    qcc_status st = qcc_source_load_file(input_path, &source);
    if (st != QCC_OK) {
        fprintf(stderr, "qcc: error: cannot read '%s': %s\n", input_path,
                qcc_status_str(st));
        return QCC_EXIT_ERROR;
    }

    (void)output_path;
    fprintf(stderr,
            "qcc: error: full translation is not implemented yet "
            "(read %zu bytes, %zu line%s from '%s')\n",
            source.size, source.line_count,
            source.line_count == 1 ? "" : "s", source.name);
    fprintf(stderr, "qcc: note: the preprocessor works — try 'qcc -E %s'; "
                    "later stages land front to back (ADR-0013)\n",
            input_path);

    qcc_source_dispose(&source);
    return QCC_EXIT_ERROR;
}

int main(int argc, char **argv)
{
    const char *input_path  = NULL;
    const char *output_path = NULL;
    int         flag_E      = 0;
    dirlist     angle       = { NULL, 0, 0 };
    dirlist     quote       = { NULL, 0, 0 };
    int         result      = QCC_EXIT_OK;

    /*
     * Manual argv walk: the option surface is small and a hand-rolled loop keeps
     * us inside portable ISO C (getopt is POSIX, not ISO — ADR-0009). Both the
     * separate ("-I dir") and attached ("-Idir") spellings are accepted.
     */
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            print_usage(stdout, "qcc");
            goto cleanup;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("qcc (Quicks) pre-release — roadmap Phase 4, ADR-0013\n");
            goto cleanup;
        }
        if (strcmp(arg, "-E") == 0) {
            flag_E = 1;
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qcc: error: -o requires an output path\n");
                result = QCC_EXIT_USAGE;
                goto cleanup;
            }
            if (output_path != NULL) {
                fprintf(stderr, "qcc: error: -o given more than once\n");
                result = QCC_EXIT_USAGE;
                goto cleanup;
            }
            output_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "-iquote") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qcc: error: -iquote requires a directory\n");
                result = QCC_EXIT_USAGE;
                goto cleanup;
            }
            if (!dirlist_push(&quote, argv[++i])) {
                goto oom;
            }
            continue;
        }
        if (strncmp(arg, "-iquote", 7) == 0) { /* -iquote<dir> attached. */
            if (!dirlist_push(&quote, arg + 7)) {
                goto oom;
            }
            continue;
        }
        if (strcmp(arg, "-I") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qcc: error: -I requires a directory\n");
                result = QCC_EXIT_USAGE;
                goto cleanup;
            }
            if (!dirlist_push(&angle, argv[++i])) {
                goto oom;
            }
            continue;
        }
        if (strncmp(arg, "-I", 2) == 0) { /* -I<dir> attached. */
            if (!dirlist_push(&angle, arg + 2)) {
                goto oom;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "qcc: error: unknown option '%s'\n", arg);
            print_usage(stderr, "qcc");
            result = QCC_EXIT_USAGE;
            goto cleanup;
        }
        if (input_path != NULL) {
            fprintf(stderr, "qcc: error: more than one input file "
                            "('%s' and '%s')\n", input_path, arg);
            result = QCC_EXIT_USAGE;
            goto cleanup;
        }
        input_path = arg;
    }

    if (input_path == NULL) {
        fprintf(stderr, "qcc: error: no input file\n");
        print_usage(stderr, "qcc");
        result = QCC_EXIT_USAGE;
        goto cleanup;
    }

    result = flag_E ? run_preprocess(input_path, output_path, &angle, &quote)
                    : report_not_implemented(input_path, output_path);
    goto cleanup;

oom:
    fprintf(stderr, "qcc: error: out of memory parsing arguments\n");
    result = QCC_EXIT_ERROR;

cleanup:
    dirlist_free(&angle);
    dirlist_free(&quote);
    return result;
}
