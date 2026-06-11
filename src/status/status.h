/*
 * qcc — status codes (the project-wide result type for qcc)
 *
 * Responsibility
 * Define the single status enum used by every fallible function in qcc, plus a
 * function to render a status as a stable, human-readable string.
 *
 * Why a dedicated status type (and not, say, returning -1 / errno)?
 *   The project's error-handling policy (Quicks-Meta/docs/standards/error-handling.md)
 *   requires explicit, named result codes returned by value, with results passed
 *   through out-parameters. This keeps control flow legible and makes it impossible
 *   to confuse "a valid result that happens to be -1" with "an error".
 *
 * Invariants (relied upon across the codebase)
 *   - QCC_OK == 0, so both `if (status)` and `if (status != QCC_OK)` mean "failed".
 *   - No failure code is 0, and every code has a distinct value.
 *
 * This is the public interface of the `status` module (one public header per
 * module — see Quicks-Meta/docs/adr/0008-directory-architecture-rules.md).
 */
#ifndef QCC_STATUS_STATUS_H
#define QCC_STATUS_STATUS_H

/*
 * qcc_status — the result of any operation that can fail.
 *
 * The numeric values are deliberately left implicit (0, 1, 2, …) except that
 * QCC_OK is pinned to 0; callers must compare against the named constants, never
 * against raw integers, so the exact values are not part of the contract.
 */
typedef enum qcc_status {
    QCC_OK = 0,                 /* Success. Always zero (see invariants above).      */

    QCC_ERR_OUT_OF_MEMORY,      /* A memory allocation failed.                       */
    QCC_ERR_IO,                 /* An I/O operation failed (open/read/write).        */
    QCC_ERR_INVALID_ARGUMENT,   /* A precondition on an argument was violated (a
                                   programming error in the caller).                 */
    QCC_ERR_NOT_FOUND,          /* A requested entity (file, symbol, …) is absent.   */
    QCC_ERR_OVERFLOW,           /* A value did not fit the target representation.    */

    /*
     * One code per pipeline stage that can reject a program (ADR-0013). Each
     * means "diagnostics were emitted via the diag module; consult them" — the
     * status itself only tells control flow *which stage* gave up.
     */
    QCC_ERR_LEX,                /* Lexical error(s) (translation phases 1-3,
                                   ISO C11 §5.1.1.2, §6.4).                          */
    QCC_ERR_PP,                 /* Preprocessing failed (phase 4, §6.10).            */
    QCC_ERR_PARSE,              /* Syntax error(s) against the C11 grammar.          */
    QCC_ERR_TYPE,               /* A constraint violation found by semantic
                                   analysis (the "Constraints" clauses of §6).       */
    QCC_ERR_IR,                 /* Lowering to or transforming the IR failed.        */
    QCC_ERR_CODEGEN,            /* The backend could not emit code (e.g. an IR
                                   construct without a selection rule yet).          */

    QCC_ERR_UNSUPPORTED         /* A well-formed request we do not implement yet.    */
} qcc_status;

/*
 * qcc_status_str — return a stable, human-readable name for a status.
 *
 * The returned pointer is to a static string literal: it is always valid, must
 * not be freed, and never changes. Intended for diagnostics and test output, not
 * for end-user error messages (those carry source locations via the diag module).
 *
 * Returns a generic "unknown status" string for any value not enumerated above,
 * so the function is total and safe even if the enum grows and a caller lags.
 */
const char *qcc_status_str(qcc_status status);

#endif /* QCC_STATUS_STATUS_H */
