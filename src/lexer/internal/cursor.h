/*
 * qcc — lexer internals: the logical-character cursor (phases 1-2).
 *
 * Responsibility
 * Provide the ONLY way the lexer reads source bytes: a reader that performs
 * translation phase 2 (line splicing, ISO C11 §5.1.1.2) on the fly, plus the
 * character classifications of the basic source character set (§5.2.1) and
 * the splice-free spelling copier. Implementing phase 2 in exactly one place
 * is what makes a splice in the middle of ANY token — identifier, pp-number,
 * string body, the two '<' of "<<" — just work in every scanner.
 *
 * Internal header (ADR-0008): only lexer/ files may include it.
 */
#ifndef QCC_LEXER_INTERNAL_CURSOR_H
#define QCC_LEXER_INTERNAL_CURSOR_H

#include <stddef.h>

#include "source/source.h"

/* Character classes (ASCII by construction, locale-independent). */
int qcc_lx_is_space_not_nl(char c);  /* space \t \v \f \r — not newline.   */
int qcc_lx_is_digit(char c);
int qcc_lx_is_hex_digit(char c);
int qcc_lx_is_ident_start(char c);   /* nondigit of §6.4.2.1 (incl. '_').  */
int qcc_lx_is_ident_cont(char c);

/*
 * Phase 2: skip every line splice (backslash immediately followed by a
 * newline; "\r\n" accepted so CRLF sources splice correctly) starting at
 * pos. Returns the physical offset of the first byte not inside a splice.
 * Sentinel-safe: qcc_source guarantees data[size] == '\0'.
 */
size_t qcc_lx_skip_splices(const qcc_source *src, size_t pos);

/*
 * Read the logical character at physical offset `pos`: skip splices, report
 * where the character actually sits (*chpos) and the offset just past it
 * (*next). At end of input returns the '\0' sentinel with *chpos == size;
 * callers distinguish a real NUL byte by chpos < size.
 */
char qcc_lx_at(const qcc_source *src, size_t pos, size_t *chpos, size_t *next);

/*
 * Copy the logical spelling of the physical span [offset, offset+length) —
 * the bytes with every splice removed — into buf (at most cap bytes, no NUL
 * appended). Returns the full logical length, always <= length.
 */
size_t qcc_lx_spelling(const qcc_source *src, size_t offset, size_t length,
                       char *buf, size_t cap);

#endif /* QCC_LEXER_INTERNAL_CURSOR_H */
