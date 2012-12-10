/*
 * Utilities for reading a HTTP body, either request or response.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http-body.h"
#include "http-error.h"
#include "istream-internal.h"
#include "fifo-buffer.h"
#include "buffered_socket.h"

#include <assert.h>
#include <limits.h>

off_t
http_body_available(const struct http_body_reader *body,
                    const struct fifo_buffer *buffer, bool partial)
{
    assert(body->rest != -2);

    if (body->rest >= 0)
        return body->rest;

    return partial
        ? (off_t)fifo_buffer_available(buffer)
        : -1;
}

gcc_pure
off_t
http_body_available2(const struct http_body_reader *body,
                     const struct buffered_socket *s, bool partial)
{
    assert(body->rest != -2);

    if (body->rest >= 0)
        return body->rest;

    return partial
        ? (off_t)buffered_socket_available(s)
        : -1;
}

/** determine how much can be read from the body */
static inline size_t
http_body_max_read(struct http_body_reader *body, size_t length)
{
    assert(body->rest != -2);

    if (body->rest != (off_t)-1 && body->rest < (off_t)length)
        /* content-length header was provided, return this value */
        return (size_t)body->rest;
    else
        /* read as much as possible, the dechunker will do the rest */
        return length;
}

static void
http_body_consumed(struct http_body_reader *body, size_t nbytes)
{
    if (body->rest < 0)
        return;

    assert((off_t)nbytes <= body->rest);

    body->rest -= (off_t)nbytes;
}

size_t
http_body_feed_body(struct http_body_reader *body,
                    const void *data, size_t length)
{
    assert(length > 0);

    length = http_body_max_read(body, length);
    size_t consumed = istream_invoke_data(&body->output, data, length);
    if (consumed > 0)
        http_body_consumed(body, consumed);

    return consumed;
}

size_t
http_body_consume_body(struct http_body_reader *body,
                       struct fifo_buffer *buffer)
{
    size_t length;
    const void *data = fifo_buffer_read(buffer, &length);
    if (data == NULL)
        return (size_t)-1;

    size_t consumed = http_body_feed_body(body, data, length);
    if (consumed > 0)
        fifo_buffer_consume(buffer, consumed);

    return consumed;
}

ssize_t
http_body_try_direct(struct http_body_reader *body, int fd,
                     enum istream_direct fd_type)
{
    assert(fd >= 0);
    assert(istream_check_direct(&body->output, fd_type));
    assert(body->output.handler->direct != NULL);

    ssize_t nbytes = istream_invoke_direct(&body->output,
                                           fd_type, fd,
                                           http_body_max_read(body, INT_MAX));
    if (nbytes > 0)
        http_body_consumed(body, (size_t)nbytes);

    return nbytes;
}

bool
http_body_socket_is_done(struct http_body_reader *body,
                         const struct buffered_socket *s)
{
    return body->rest != -1 &&
        (http_body_eof(body) ||
         (off_t)buffered_socket_available(s) >= body->rest);
}

bool
http_body_socket_eof(struct http_body_reader *body, size_t remaining)
{
#ifndef NDEBUG
    body->socket_eof = true;
#endif

    if (body->rest == -1) {
        if (remaining > 0) {
            /* serve the rest of the buffer, then end the body
               stream */
            body->rest = remaining;
            return true;
        }

        /* the socket is closed, which ends the body */
        istream_deinit_eof(&body->output);
        return false;
    } else if (body->rest == (off_t)remaining || body->rest == -2) {
        if (remaining > 0)
            /* serve the rest of the buffer, then end the body
               stream */
            return true;

        istream_deinit_eof(&body->output);
        return false;
    } else {
        /* something has gone wrong: either not enough or too much
           data left in the buffer */
        GError *error = g_error_new_literal(http_quark(), 0,
                                            "premature end of socket");
        istream_deinit_abort(&body->output, error);
        return false;
    }
}

static void
http_body_dechunker_eof(void *ctx)
{
    struct http_body_reader *body = ctx;

    assert(body->chunked);
    assert(body->rest == -1 || (body->socket_eof && body->rest >= 0));

    body->rest = -2;
}

struct istream *
http_body_init(struct http_body_reader *body,
               const struct istream_class *stream, struct pool *stream_pool,
               struct pool *pool, off_t content_length, bool chunked)
{
    assert(pool_contains(stream_pool, body, sizeof(*body)));

    istream_init(&body->output, stream, stream_pool);
    body->rest = content_length;

#ifndef NDEBUG
    body->chunked = chunked;
    body->socket_eof = false;
#endif

    struct istream *istream = http_body_istream(body);
    if (chunked) {
        assert(content_length == (off_t)-1);

        istream = istream_dechunk_new(pool, istream,
                                      http_body_dechunker_eof, body);
    }

    return istream;
}
