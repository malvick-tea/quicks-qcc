/*
 * qcc — command-line entry point.
 *
 * What works today
 *   The front end runs through translation phases 1-7 (lexer, preprocessor,
 *   convert; ADR-0013 pipeline) and the parser recognises the whole §6.9
 *   translation unit (ADR-0019/0022/0023/0024). Three developer-facing modes
 *   expose the stages: `qcc -E <in.c>` preprocesses a translation unit and writes
 *   the resulting text (phase 4 — every §6.10 directive, macro expansion, and
 *   `#include`); `qcc -dump-tokens <in.c>` runs the whole pipeline through
 *   `convert` (phases 5-7) and writes the token stream — kinds, evaluated constant
 *   values, and string contents — one token per line; `qcc -dump-ast <in.c>`
 *   parses the translation unit and writes each external declaration's parse tree
 *   as an S-expression. All three aid inspection and bootstrap differential
 *   testing. The later stages (semantic analysis, codegen) land front to back;
 *   without a mode flag, qcc reports that full translation is not implemented yet
 *   rather than pretending to compile.
 *
 * Exit codes (stable contract, shared shape with qas/qld)
 *   0  success
 *   1  the input produced errors, or an I/O error occurred
 *   2  usage error (bad/missing arguments)
 *
 * This file is the only place that talks to argv and stdout/stderr for the tool;
 * the library modules (qcc_core) stay free of I/O policy so they remain testable.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast/ast.h"
#include "convert/convert.h"
#include "diag/diag.h"
#include "parser/parser.h"
#include "pp/pp.h"
#include "pp/render.h"
#include "source/source.h"
#include "status/status.h"
#include "symtab/symtab.h"
#include "token/token.h"
#include "type/type.h"

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
            "  %s -dump-tokens [...] <in.c>\n"
            "                             Dump the token stream (phases 5-7)\n"
            "  %s -dump-ast [...] <in.c>  Dump the parse tree (the §6.9 translation\n"
            "                             unit) as S-expressions, one per line\n"
            "  %s [-o <out.s>] <in.c>     Compile <in.c> to x86-64 assembly (Intel\n"
            "                             syntax, for qas) — not yet implemented\n"
            "  %s --help                  Show this help\n"
            "  %s --version               Show version information\n"
            "\n"
            "Options:\n"
            "  -E            Preprocess only; write the result to stdout (or -o).\n"
            "  -dump-tokens  Run the full front end and print one token per line.\n"
            "  -dump-ast     Parse the translation unit and print its syntax tree.\n"
            "  -I <dir>      Add <dir> to the angle (<...>) include search path.\n"
            "  -iquote <dir> Add <dir> to the quote (\"...\") include search path.\n"
            "  -o <out>      Write output to <out> instead of stdout / <in>.s.\n",
            prog, prog, prog, prog, prog, prog);
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

/* Print a string literal's decoded value: a quoted, escaped preview for the
   narrow encodings, or a list of hex code units for the wide ones. */
static void dump_string_value(FILE *outf, const qcc_token *tk)
{
    fprintf(outf, "%s[%zu] ", qcc_char_encoding_str(tk->char_encoding),
            tk->str_len);

    if (tk->char_encoding == QCC_ENC_PLAIN || tk->char_encoding == QCC_ENC_UTF8) {
        const unsigned char *d = (const unsigned char *)tk->str_data;
        fputc('"', outf);
        for (size_t i = 0; i < tk->str_len; ++i) {
            unsigned char b = d[i];
            switch (b) {
            case '\n': fputs("\\n", outf);  break;
            case '\t': fputs("\\t", outf);  break;
            case '\r': fputs("\\r", outf);  break;
            case '"':  fputs("\\\"", outf); break;
            case '\\': fputs("\\\\", outf); break;
            default:
                if (b >= 0x20u && b < 0x7Fu) {
                    fputc((int)b, outf);
                } else {
                    fprintf(outf, "\\x%02X", b);
                }
            }
        }
        fputc('"', outf);
        return;
    }

    fputc('{', outf);
    for (size_t i = 0; i < tk->str_len; ++i) {
        unsigned long v = (tk->char_encoding == QCC_ENC_CHAR16)
                              ? (unsigned long)((const uint16_t *)tk->str_data)[i]
                              : (unsigned long)((const uint32_t *)tk->str_data)[i];
        fprintf(outf, "%s0x%lX", (i != 0) ? " " : "", v);
    }
    fputc('}', outf);
}

/* Write one token's "line:col<TAB>kind<TAB>detail" dump line. The detail is
   chosen by category so the listing shows what convert resolved (the keyword, the
   evaluated constant value and type, the decoded string), not just the lexeme. */
static void dump_one_token(FILE *outf, const qcc_token *tk)
{
    fprintf(outf, "%u:%u\t%s\t", tk->line, tk->column,
            qcc_token_kind_name(tk->kind));

    switch (tk->kind) {
    case QCC_TOKEN_KEYWORD:
        fputs(qcc_keyword_str(tk->keyword), outf);
        break;
    case QCC_TOKEN_IDENTIFIER:
        fprintf(outf, "%.*s", (int)tk->spelling_len, tk->spelling);
        break;
    case QCC_TOKEN_PUNCT:
        fputs(qcc_punct_str(tk->punct), outf);
        break;
    case QCC_TOKEN_INTEGER:
        fprintf(outf, "%llu (%s)", (unsigned long long)tk->int_value,
                qcc_int_type_name(tk->int_type));
        break;
    case QCC_TOKEN_FLOATING:
        fprintf(outf, "%g (%s)", tk->float_value,
                qcc_float_type_name(tk->float_type));
        break;
    case QCC_TOKEN_CHAR:
        fprintf(outf, "%lld (%s)", (long long)(int64_t)tk->int_value,
                qcc_char_encoding_str(tk->char_encoding));
        break;
    case QCC_TOKEN_STRING:
        dump_string_value(outf, tk);
        break;
    case QCC_TOKEN_EOF:
        fputs("<eof>", outf);
        break;
    }
    fputc('\n', outf);
}

/* Write the whole token stream to `output_path` (NULL => stdout). Returns 1 on
   success, 0 on an I/O failure (message already printed). */
static int write_token_dump(const qcc_token_list *toks, const char *output_path)
{
    FILE *outf      = stdout;
    int   close_out = 0;
    if (output_path != NULL) {
        outf = fopen(output_path, "wb");
        if (outf == NULL) {
            fprintf(stderr, "qcc: error: cannot open '%s' for writing\n",
                    output_path);
            return 0;
        }
        close_out = 1;
    }

    for (size_t i = 0; i < toks->count; ++i) {
        dump_one_token(outf, &toks->items[i]);
    }

    int ok = 1;
    if (ferror(outf)) {
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
    return ok;
}

/* The `qcc -dump-tokens` path: run lexer -> preprocessor -> convert and write the
   resulting token stream. Mirrors run_preprocess but carries the stream one phase
   further (phases 5-7, §6.4) before rendering. */
static int run_dump(const char *input_path, const char *output_path,
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
        qcc_ptok_list pt;
        qcc_ptok_list_init(&pt);
        st = qcc_pp_run(&pp, &source, &pt);
        if (st != QCC_OK) {
            fprintf(stderr, "qcc: error: preprocessing failed: %s\n",
                    qcc_status_str(st));
            exit_code = QCC_EXIT_ERROR;
        } else {
            qcc_convert cv;
            st = qcc_convert_init(&cv, &diags);
            if (st != QCC_OK) {
                fprintf(stderr, "qcc: error: %s\n", qcc_status_str(st));
                exit_code = QCC_EXIT_ERROR;
            } else {
                qcc_token_list toks;
                qcc_token_list_init(&toks);
                st = qcc_convert_run(&cv, &pt, &toks);
                if (st != QCC_OK) {
                    fprintf(stderr, "qcc: error: token conversion failed: %s\n",
                            qcc_status_str(st));
                    exit_code = QCC_EXIT_ERROR;
                } else if (!write_token_dump(&toks, output_path)) {
                    exit_code = QCC_EXIT_ERROR;
                }
                qcc_token_list_dispose(&toks);
                qcc_convert_dispose(&cv);
            }
        }
        qcc_ptok_list_dispose(&pt);
    }

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

/* Write one external declaration's S-expression dump line. Returns 1 on success,
   0 on a dump (out-of-memory) failure (message already printed). */
static int write_ast_decl(FILE *outf, const qcc_extern_decl *ed)
{
    char  *text = NULL;
    size_t len  = 0;
    if (qcc_extern_decl_dump(ed, &text, &len) != QCC_OK) {
        fprintf(stderr, "qcc: error: out of memory rendering the syntax tree\n");
        return 0;
    }
    if (len > 0) {
        fwrite(text, 1, len, outf);
    }
    fputc('\n', outf);
    free(text);
    return 1;
}

/* Parse the translation unit in `toks` and write each external declaration's
   parse tree as an S-expression to `output_path` (NULL => stdout). Parse errors go
   to `diags` (reported by the caller). Returns a QCC_EXIT_* code. */
static int parse_and_dump_ast(const qcc_token_list *toks, const char *output_path,
                              qcc_diag_sink *diags)
{
    FILE *outf      = stdout;
    int   close_out = 0;
    if (output_path != NULL) {
        outf = fopen(output_path, "wb");
        if (outf == NULL) {
            fprintf(stderr, "qcc: error: cannot open '%s' for writing\n",
                    output_path);
            return QCC_EXIT_ERROR;
        }
        close_out = 1;
    }

    qcc_type_ctx tc;
    qcc_type_ctx_init(&tc);
    qcc_symtab sym;
    qcc_symtab_init(&sym);
    qcc_ast ast;
    qcc_ast_init(&ast);
    qcc_parser parser;
    qcc_parser_init(&parser, toks->items, toks->count, &ast, &tc, &sym, diags);

    int exit_code = QCC_EXIT_OK;
    while (!qcc_parser_at_end(&parser)) {
        qcc_extern_decl ed;
        qcc_status      pst = qcc_parse_external_declaration(&parser, &ed);
        if (pst != QCC_OK) {
            /* A QCC_ERR_PARSE diagnostic is already in the sink; OOM is not. */
            if (pst == QCC_ERR_OUT_OF_MEMORY) {
                fprintf(stderr, "qcc: error: out of memory parsing\n");
            }
            exit_code = QCC_EXIT_ERROR;
            break;
        }
        if (!write_ast_decl(outf, &ed)) {
            exit_code = QCC_EXIT_ERROR;
            break;
        }
    }

    if (exit_code == QCC_EXIT_OK && ferror(outf)) {
        fprintf(stderr, "qcc: error: writing output failed\n");
        exit_code = QCC_EXIT_ERROR;
    }
    if (close_out) {
        if (fclose(outf) != 0) {
            exit_code = QCC_EXIT_ERROR;
        }
    } else {
        fflush(outf);
    }

    qcc_ast_dispose(&ast);
    qcc_symtab_dispose(&sym);
    qcc_type_ctx_dispose(&tc);
    return exit_code;
}

/* The `qcc -dump-ast` path: run the front end (phases 1-7), parse the translation
   unit (§6.9), and write each external declaration's parse tree as a deterministic
   S-expression — one per line — for inspection and bootstrap differential testing.
   Mirrors run_dump, carrying the stream one subsystem further (the parser). */
static int run_dump_ast(const char *input_path, const char *output_path,
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
        qcc_ptok_list pt;
        qcc_ptok_list_init(&pt);
        st = qcc_pp_run(&pp, &source, &pt);
        if (st != QCC_OK) {
            fprintf(stderr, "qcc: error: preprocessing failed: %s\n",
                    qcc_status_str(st));
            exit_code = QCC_EXIT_ERROR;
        } else {
            qcc_convert cv;
            st = qcc_convert_init(&cv, &diags);
            if (st != QCC_OK) {
                fprintf(stderr, "qcc: error: %s\n", qcc_status_str(st));
                exit_code = QCC_EXIT_ERROR;
            } else {
                qcc_token_list toks;
                qcc_token_list_init(&toks);
                st = qcc_convert_run(&cv, &pt, &toks);
                if (st != QCC_OK) {
                    fprintf(stderr, "qcc: error: token conversion failed: %s\n",
                            qcc_status_str(st));
                    exit_code = QCC_EXIT_ERROR;
                } else {
                    exit_code = parse_and_dump_ast(&toks, output_path, &diags);
                }
                qcc_token_list_dispose(&toks);
                qcc_convert_dispose(&cv);
            }
        }
        qcc_ptok_list_dispose(&pt);
    }

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
    const char *input_path   = NULL;
    const char *output_path  = NULL;
    int         flag_E       = 0;
    int         flag_dump    = 0;
    int         flag_dumpast = 0;
    dirlist     angle        = { NULL, 0, 0 };
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
        if (strcmp(arg, "-dump-tokens") == 0) {
            flag_dump = 1;
            continue;
        }
        if (strcmp(arg, "-dump-ast") == 0) {
            flag_dumpast = 1;
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

    if (flag_E + flag_dump + flag_dumpast > 1) {
        fprintf(stderr, "qcc: error: -E, -dump-tokens, and -dump-ast are "
                        "mutually exclusive\n");
        result = QCC_EXIT_USAGE;
        goto cleanup;
    }

    if (flag_dumpast) {
        result = run_dump_ast(input_path, output_path, &angle, &quote);
    } else if (flag_dump) {
        result = run_dump(input_path, output_path, &angle, &quote);
    } else if (flag_E) {
        result = run_preprocess(input_path, output_path, &angle, &quote);
    } else {
        result = report_not_implemented(input_path, output_path);
    }
    goto cleanup;

oom:
    fprintf(stderr, "qcc: error: out of memory parsing arguments\n");
    result = QCC_EXIT_ERROR;

cleanup:
    dirlist_free(&angle);
    dirlist_free(&quote);
    return result;
}
