/*
 * qcc — status codes: implementation of qcc_status_str.
 *
 * The switch enumerates every status explicitly (no `default` that hides missing
 * cases): if a new status is added to the enum and not handled here, a
 * conforming compiler with -Wswitch/-Wswitch-enum will flag it, which is exactly
 * the early warning we want. A final `return` after the switch handles
 * out-of-range integer values cast to the enum type, keeping the function total
 * (see the header's contract).
 */
#include "status/status.h"

const char *qcc_status_str(qcc_status status)
{
    switch (status) {
    case QCC_OK:                   return "ok";
    case QCC_ERR_OUT_OF_MEMORY:    return "out of memory";
    case QCC_ERR_IO:               return "I/O error";
    case QCC_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QCC_ERR_NOT_FOUND:        return "not found";
    case QCC_ERR_OVERFLOW:         return "value overflow";
    case QCC_ERR_LEX:              return "lexical error";
    case QCC_ERR_PP:               return "preprocessing error";
    case QCC_ERR_PARSE:            return "syntax error";
    case QCC_ERR_TYPE:             return "type/constraint error";
    case QCC_ERR_IR:               return "IR error";
    case QCC_ERR_CODEGEN:          return "code generation error";
    case QCC_ERR_UNSUPPORTED:      return "unsupported";
    }

    /*
     * Reached only if `status` holds an integer outside the enumerated set
     * (e.g. a corrupted value). Returning a sentinel keeps the function safe.
     */
    return "unknown status";
}
