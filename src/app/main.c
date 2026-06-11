/*
 * qcc — command-line entry point.
 *
 * What works today
 *   The compiler is at the very start of Phase 4 (Quicks-Meta roadmap; pipeline
 *   architecture in ADR-0013). The CLI validates its arguments, loads the input
 *   source (exercising the real source module: exact bytes, line index for
 *   diagnostics), and then states honestly that translation is not implemented
 *   yet. Stages appear here as they land, front to back.
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
#include <string.h>

#include "source/source.h"
#include "status/status.h"

/* Exit codes, named so call sites read clearly (no magic numbers). */
enum {
    QCC_EXIT_OK    = 0,
    QCC_EXIT_ERROR = 1,
    QCC_EXIT_USAGE = 2
};

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "qcc — the Quicks C compiler\n"
            "\n"
            "Usage:\n"
            "  %s [-o <out.s>] <in.c>     Compile <in.c> to x86-64 assembly (Intel\n"
            "                             syntax, for qas)\n"
            "  %s --help                  Show this help\n"
            "  %s --version               Show version information\n"
            "\n"
            "If -o is omitted, the assembly is written next to the input with a\n"
            "'.s' extension.\n",
            prog, prog, prog);
}

int main(int argc, char **argv)
{
    const char *input_path  = NULL;
    const char *output_path = NULL;

    /*
     * Manual argv walk: the option surface is tiny and a hand-rolled loop keeps
     * us inside portable ISO C (getopt is POSIX, not ISO — ADR-0009).
     */
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            print_usage(stdout, "qcc");
            return QCC_EXIT_OK;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("qcc (Quicks) pre-release — roadmap Phase 4, ADR-0013\n");
            return QCC_EXIT_OK;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qcc: error: -o requires an output path\n");
                return QCC_EXIT_USAGE;
            }
            if (output_path != NULL) {
                fprintf(stderr, "qcc: error: -o given more than once\n");
                return QCC_EXIT_USAGE;
            }
            output_path = argv[++i];
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "qcc: error: unknown option '%s'\n", arg);
            print_usage(stderr, "qcc");
            return QCC_EXIT_USAGE;
        }
        if (input_path != NULL) {
            fprintf(stderr, "qcc: error: more than one input file "
                            "('%s' and '%s')\n", input_path, arg);
            return QCC_EXIT_USAGE;
        }
        input_path = arg;
    }

    if (input_path == NULL) {
        fprintf(stderr, "qcc: error: no input file\n");
        print_usage(stderr, "qcc");
        return QCC_EXIT_USAGE;
    }

    /* Load the input now so "file does not exist" fails today, not at the
       (future) point where the lexer first pulls bytes. */
    qcc_source source;
    qcc_status st = qcc_source_load_file(input_path, &source);
    if (st != QCC_OK) {
        fprintf(stderr, "qcc: error: cannot read '%s': %s\n", input_path,
                qcc_status_str(st));
        return QCC_EXIT_ERROR;
    }

    /*
     * Honesty over pretense: no translation stage exists yet, so say so instead
     * of silently succeeding. The unreferenced output path is part of the
     * validated interface already, so scripts written today keep working as the
     * pipeline lands.
     */
    (void)output_path;
    fprintf(stderr,
            "qcc: error: translation is not implemented yet "
            "(read %zu bytes, %zu line%s from '%s')\n",
            source.size, source.line_count,
            source.line_count == 1 ? "" : "s", source.name);
    fprintf(stderr, "qcc: note: the pipeline lands stage by stage; "
                    "see ADR-0013 and the roadmap (Phase 4)\n");

    qcc_source_dispose(&source);
    return QCC_EXIT_ERROR;
}
