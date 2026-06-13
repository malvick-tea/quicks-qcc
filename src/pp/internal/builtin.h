/*
 * qcc — preprocessor internals: predefined macros (ISO C11 §6.10.8)
 *
 * Responsibility
 * Install the standard predefined macros into a freshly initialized
 * preprocessor: the position-dependent __LINE__ and __FILE__ (tagged as
 * builtins, computed at each use) and the fixed-value macros __STDC__,
 * __STDC_VERSION__, __STDC_HOSTED__, __DATE__, and __TIME__ (ordinary object-
 * like macros whose replacement is built once at startup).
 *
 * Internal header (ADR-0008): only pp/ files include it.
 */
#ifndef QCC_PP_INTERNAL_BUILTIN_H
#define QCC_PP_INTERNAL_BUILTIN_H

#include "pp/pp.h"
#include "status/status.h"

/*
 * Install all predefined macros into pp->macros. Called once from qcc_pp_init.
 * Returns QCC_OK or QCC_ERR_OUT_OF_MEMORY.
 */
qcc_status qcc_pp_install_builtins(qcc_pp *pp);

#endif /* QCC_PP_INTERNAL_BUILTIN_H */
