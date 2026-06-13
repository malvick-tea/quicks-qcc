/*
 * qcc — preprocessor internals: the token-input stream (implementation).
 *
 * See stream.h for the contract. The stack is a singly linked list of heap
 * frames with `top` at the head. A LEXER frame materializes the lexer's
 * span-backed tokens into qcc_ptok here (the one place that happens); a TOKENS
 * frame yields pre-materialized tokens. Exhausted frames are popped and freed,
 * and the bottom lexer's EOF is the stream's terminal EOF.
 */
#include "pp/internal/stream.h"

#include <stdlib.h>

#define QCC_PP_STREAM_SCRATCH_INITIAL ((size_t)128u)

/* Private helpers. */

/* Allocate and push a frame of the given kind; returns it or NULL on OOM. */
static qcc_pp_input *push_frame(qcc_pp_stream *stream, qcc_pp_input_kind kind)
{
    qcc_pp_input *frame = (qcc_pp_input *)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        return NULL;
    }
    frame->kind  = kind;
    frame->below = stream->top;
    stream->top  = frame;
    return frame;
}

/* Pop and free the top frame (does not touch borrowed sources/token arrays). */
static void pop_frame(qcc_pp_stream *stream)
{
    qcc_pp_input *frame = stream->top;
    if (frame != NULL) {
        stream->top = frame->below;
        free(frame);
    }
}

/* A synthetic EOF token, used once the stack is fully exhausted. */
static void make_eof(qcc_ptok *out)
{
    out->kind          = QCC_PP_TOKEN_EOF;
    out->punct         = (qcc_punct)0;
    out->spelling      = "";
    out->spelling_len  = 0;
    out->source        = NULL;
    out->offset        = 0;
    out->line          = 0;
    out->column        = 0;
    out->presumed_line = 0;
    out->presumed_file = NULL;
    out->leading_space = 0;
    out->at_line_start = 1;
    out->hideset       = NULL;
}

/*
 * Materialize one lexer token (span-backed) into *out: recover its splice-free
 * spelling into the stream's scratch buffer (grown as needed) and intern it,
 * then copy kind/punct/provenance/flags with an empty hide set. The presumed
 * line/file (§6.10.4) are stamped from the file input's current #line shift, so
 * `__LINE__`/`__FILE__` read them with no further lookup (ADR-0016). Returns
 * QCC_OK or QCC_ERR_OUT_OF_MEMORY.
 */
static qcc_status materialize(qcc_pp_stream *stream, const qcc_pp_input *frame,
                              const qcc_pp_token *in, qcc_ptok *out)
{
    const qcc_source *src = frame->source;

    if (in->length > stream->scratch_cap) {
        char *grown = (char *)realloc(stream->scratch, in->length);
        if (grown == NULL) {
            return QCC_ERR_OUT_OF_MEMORY;
        }
        stream->scratch     = grown;
        stream->scratch_cap = in->length;
    }

    size_t      len = qcc_lexer_token_spelling(src, in, stream->scratch,
                                               stream->scratch_cap);
    const char *interned = qcc_pp_intern(stream->pp, stream->scratch, len);
    if (interned == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }

    out->kind          = in->kind;
    out->punct         = in->punct;
    out->spelling      = interned;
    out->spelling_len  = len;
    out->source        = src;
    out->offset        = in->offset;
    out->line          = in->line;
    out->column        = in->column;
    out->presumed_line =
        (uint32_t)((int64_t)in->line + (int64_t)frame->presumed_delta);
    out->presumed_file = frame->presumed_file;
    out->leading_space = in->leading_space;
    out->at_line_start = in->at_line_start;
    out->hideset       = NULL;
    return QCC_OK;
}

/* Public interface. */

/* Shared field setup for both init variants; allocates the scratch buffer. */
static qcc_status common_init(qcc_pp_stream *stream, qcc_pp *pp)
{
    stream->pp                     = pp;
    stream->top                    = NULL;
    stream->has_pending            = 0;
    stream->pending_from_expansion = 0;
    stream->scratch                = (char *)malloc(QCC_PP_STREAM_SCRATCH_INITIAL);
    stream->scratch_cap            = QCC_PP_STREAM_SCRATCH_INITIAL;
    if (stream->scratch == NULL) {
        stream->scratch_cap = 0;
        return QCC_ERR_OUT_OF_MEMORY;
    }
    return QCC_OK;
}

qcc_status qcc_pp_stream_init(qcc_pp_stream *stream, qcc_pp *pp,
                              const qcc_source *source)
{
    if (stream == NULL || pp == NULL || source == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_status st = common_init(stream, pp);
    if (st != QCC_OK) {
        return st;
    }

    qcc_pp_input *frame = push_frame(stream, QCC_PP_INPUT_LEXER);
    if (frame == NULL) {
        free(stream->scratch);
        stream->scratch = NULL;
        return QCC_ERR_OUT_OF_MEMORY;
    }
    frame->source = source;
    qcc_lexer_init(&frame->lexer, source, pp->diags);
    return QCC_OK;
}

qcc_status qcc_pp_stream_init_tokens(qcc_pp_stream *stream, qcc_pp *pp,
                                     const qcc_ptok *toks, size_t count)
{
    if (stream == NULL || pp == NULL || (toks == NULL && count != 0)) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_status st = common_init(stream, pp);
    if (st != QCC_OK) {
        return st;
    }
    if (count != 0) {
        qcc_pp_input *frame = push_frame(stream, QCC_PP_INPUT_TOKENS);
        if (frame == NULL) {
            free(stream->scratch);
            stream->scratch = NULL;
            return QCC_ERR_OUT_OF_MEMORY;
        }
        frame->toks  = toks;
        frame->count = count;
        frame->pos   = 0;
    }
    return QCC_OK;
}

qcc_status qcc_pp_stream_unget(qcc_pp_stream *stream, const qcc_ptok *tok,
                               int from_expansion)
{
    if (stream == NULL || tok == NULL || stream->has_pending) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    stream->pending                = *tok;
    stream->has_pending            = 1;
    stream->pending_from_expansion = from_expansion;
    return QCC_OK;
}

void qcc_pp_stream_dispose(qcc_pp_stream *stream)
{
    if (stream == NULL) {
        return;
    }
    while (stream->top != NULL) {
        pop_frame(stream);
    }
    free(stream->scratch);
    stream->scratch     = NULL;
    stream->scratch_cap = 0;
    stream->pp          = NULL;
}

qcc_status qcc_pp_stream_next(qcc_pp_stream *stream, qcc_ptok *out,
                              int *from_expansion)
{
    if (stream == NULL || out == NULL || from_expansion == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }

    if (stream->has_pending) {
        *out                = stream->pending;
        *from_expansion     = stream->pending_from_expansion;
        stream->has_pending = 0;
        return QCC_OK;
    }

    for (;;) {
        if (stream->top == NULL) {
            make_eof(out);
            *from_expansion = 0;
            return QCC_OK; /* Fully exhausted: repeat EOF forever. */
        }

        if (stream->top->kind == QCC_PP_INPUT_TOKENS) {
            qcc_pp_input *frame = stream->top;
            if (frame->pos < frame->count) {
                *out            = frame->toks[frame->pos++];
                *from_expansion = 1;
                return QCC_OK;
            }
            pop_frame(stream); /* Replacement exhausted; resume what's below. */
            continue;
        }

        /* LEXER input. */
        qcc_pp_input *frame = stream->top;
        qcc_pp_token  raw;
        qcc_status    st = qcc_lexer_next(&frame->lexer, &raw);
        if (st != QCC_OK) {
            return st;
        }

        if (raw.kind == QCC_PP_TOKEN_EOF) {
            if (frame->below == NULL) {
                /* Outermost file ended: this is the translation unit's EOF. */
                st = materialize(stream, frame, &raw, out);
                if (st != QCC_OK) {
                    return st;
                }
                pop_frame(stream); /* top becomes NULL; later calls repeat EOF. */
                *from_expansion = 0;
                return QCC_OK;
            }
            pop_frame(stream); /* Included file ended; resume the includer. */
            continue;
        }

        st = materialize(stream, frame, &raw, out);
        if (st != QCC_OK) {
            return st;
        }
        *from_expansion = 0;
        return QCC_OK;
    }
}

qcc_status qcc_pp_stream_push_tokens(qcc_pp_stream *stream, const qcc_ptok *toks,
                                     size_t count)
{
    if (stream == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return QCC_OK; /* Nothing to rescan. */
    }
    qcc_pp_input *frame = push_frame(stream, QCC_PP_INPUT_TOKENS);
    if (frame == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    frame->toks  = toks;
    frame->count = count;
    frame->pos   = 0;
    return QCC_OK;
}

qcc_status qcc_pp_stream_push_source(qcc_pp_stream *stream,
                                     const qcc_source *source)
{
    if (stream == NULL || source == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    qcc_pp_input *frame = push_frame(stream, QCC_PP_INPUT_LEXER);
    if (frame == NULL) {
        return QCC_ERR_OUT_OF_MEMORY;
    }
    frame->source = source;
    qcc_lexer_init(&frame->lexer, source, stream->pp->diags);
    return QCC_OK;
}

const qcc_source *qcc_pp_stream_current_source(const qcc_pp_stream *stream)
{
    if (stream == NULL) {
        return NULL;
    }
    for (const qcc_pp_input *f = stream->top; f != NULL; f = f->below) {
        if (f->kind == QCC_PP_INPUT_LEXER) {
            return f->source;
        }
    }
    return NULL;
}

size_t qcc_pp_stream_lexer_depth(const qcc_pp_stream *stream)
{
    if (stream == NULL) {
        return 0;
    }
    size_t depth = 0;
    for (const qcc_pp_input *f = stream->top; f != NULL; f = f->below) {
        if (f->kind == QCC_PP_INPUT_LEXER) {
            ++depth;
        }
    }
    return depth;
}

qcc_status qcc_pp_stream_next_header(qcc_pp_stream *stream, qcc_ptok *out,
                                     int *from_expansion)
{
    if (stream == NULL || out == NULL || from_expansion == NULL) {
        return QCC_ERR_INVALID_ARGUMENT;
    }
    /* A header-name is only produced by a live file lexer with no pending
       pushback (§6.4.7). A real #include line is exactly that; otherwise read
       normally and let the caller treat the result as a computed include. */
    if (stream->has_pending || stream->top == NULL ||
        stream->top->kind != QCC_PP_INPUT_LEXER) {
        return qcc_pp_stream_next(stream, out, from_expansion);
    }

    qcc_pp_input *frame = stream->top;
    qcc_lexer_set_header_mode(&frame->lexer, 1);
    qcc_status st = qcc_pp_stream_next(stream, out, from_expansion);
    /* Reset the mode for subsequent reads, but only if the frame is still the
       top input: a frame that reached EOF was popped and freed by next() (a
       header-name read of a real directive line never reaches EOF first). */
    if (stream->top == frame) {
        qcc_lexer_set_header_mode(&frame->lexer, 0);
    }
    return st;
}

/* The topmost file (lexer) input, skipping macro-rescan inputs, or NULL. */
static qcc_pp_input *top_lexer(qcc_pp_stream *stream)
{
    for (qcc_pp_input *f = stream->top; f != NULL; f = f->below) {
        if (f->kind == QCC_PP_INPUT_LEXER) {
            return f;
        }
    }
    return NULL;
}

void qcc_pp_stream_set_presumed_line(qcc_pp_stream *stream, int32_t delta)
{
    if (stream == NULL) {
        return;
    }
    qcc_pp_input *frame = top_lexer(stream);
    if (frame != NULL) {
        frame->presumed_delta = delta;
    }
}

void qcc_pp_stream_set_presumed_file(qcc_pp_stream *stream, const char *file)
{
    if (stream == NULL) {
        return;
    }
    qcc_pp_input *frame = top_lexer(stream);
    if (frame != NULL) {
        frame->presumed_file = file;
    }
}
