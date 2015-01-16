/*
 * An istream receiving data from a socket.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_socket.hxx"
#include "istream-internal.h"
#include "istream_buffer.hxx"
#include "pool.hxx"
#include "buffered_io.hxx"
#include "pevent.hxx"
#include "gerrno.h"
#include "fb_pool.hxx"
#include "SliceFifoBuffer.hxx"
#include "util/Cast.hxx"

#include <event.h>
#include <errno.h>
#include <string.h>

struct istream_socket {
    struct istream output;

    /**
     * The socket descriptor.  Will be set to -1 when the stream is
     * closed.
     */
    int fd;
    enum istream_direct fd_type;

    const struct istream_socket_handler *handler;
    void *handler_ctx;

    SliceFifoBuffer buffer;

    struct event event;
};

static inline bool
socket_valid(const struct istream_socket *s)
{
    assert(s != nullptr);

    return s->fd >= 0;
}

static void
socket_schedule_read(struct istream_socket *s)
{
    assert(socket_valid(s));
    assert(!s->buffer.IsFull());

    p_event_add(&s->event, nullptr, s->output.pool, "istream_socket");
}

/**
 * @return true if there is still data in the buffer (or if the stream
 * has been closed), false if the buffer is empty
 */
static bool
socket_buffer_consume(struct istream_socket *s)
{
    assert(socket_valid(s));
    assert(s->buffer.IsDefined());

    if (gcc_likely(!s->buffer.IsFull() ||
                   s->handler->full == nullptr))
        /* quick path without an additional pool reference */
        return istream_buffer_consume(&s->output, s->buffer) > 0;

    pool_ref(s->output.pool);
    const bool full = istream_buffer_send(&s->output, s->buffer) == 0 &&
        socket_valid(s);
    const bool empty = !full && socket_valid(s) && s->buffer.IsEmpty();
    pool_unref(s->output.pool);

    if (gcc_unlikely(full && !s->handler->full(s->handler_ctx)))
        /* stream has been closed */
        return true;

    return !empty;
}

/**
 * @return true if data was consumed, false if the istream handler is
 * blocking (or if the stream has been closed)
 */
static bool
socket_buffer_send(struct istream_socket *s)
{
    assert(socket_valid(s));
    assert(s->buffer.IsDefined());

    if (gcc_likely(!s->buffer.IsFull() ||
                   s->handler->full == nullptr))
        /* quick path without an additional pool reference */
        return istream_buffer_send(&s->output, s->buffer) > 0;

    pool_ref(s->output.pool);
    const bool consumed = istream_buffer_send(&s->output, s->buffer) > 0;
    const bool full = !consumed && socket_valid(s);
    pool_unref(s->output.pool);

    if (full)
        s->handler->full(s->handler_ctx);

    return consumed;
}

static void
socket_try_direct(struct istream_socket *s)
{
    assert(socket_valid(s));

    if (s->buffer.IsDefined()) {
        if (socket_buffer_consume(s))
            return;

        s->buffer.Free(fb_pool_get());
    }

    ssize_t nbytes = istream_invoke_direct(&s->output, s->fd_type, s->fd,
                                           G_MAXINT);
    if (G_LIKELY(nbytes > 0)) {
        /* schedule the next read */
        socket_schedule_read(s);
    } else if (nbytes == 0) {
        if (s->handler->depleted(s->handler_ctx) &&
            s->handler->finished(s->handler_ctx)) {
            s->fd = -1;
            istream_deinit_eof(&s->output);
        }
    } else if (nbytes == ISTREAM_RESULT_BLOCKING ||
               nbytes == ISTREAM_RESULT_CLOSED) {
        /* either the destination fd blocks (-2) or the stream (and
           the whole connection) has been closed during the direct()
           callback (-3); no further checks */
        return;
    } else if (errno == EAGAIN) {
        /* wait for the socket */
        socket_schedule_read(s);
    } else {
        const int e = errno;
        if (!s->handler->error(e, s->handler_ctx))
            return;

        GError *error = new_error_errno_msg2(e, "recv error");
        s->fd = -1;
        istream_deinit_abort(&s->output, error);
    }
}

static void
socket_try_buffered(struct istream_socket *s)
{
    assert(socket_valid(s));

    if (s->buffer.IsNull())
        s->buffer.Allocate(fb_pool_get());
    else if (socket_buffer_consume(s))
        return;

    assert(!s->buffer.IsFull());

    ssize_t nbytes = recv_to_buffer(s->fd, s->buffer, G_MAXINT);
    if (G_LIKELY(nbytes > 0)) {
        if (socket_buffer_send(s))
            socket_schedule_read(s);
    } else if (nbytes == 0) {
        if (s->handler->depleted(s->handler_ctx) &&
            s->handler->finished(s->handler_ctx)) {
            s->buffer.Free(fb_pool_get());
            s->fd = -1;
            istream_deinit_eof(&s->output);
        }
    } else if (errno == EAGAIN) {
        socket_schedule_read(s);
    } else {
        const int e = errno;

        s->buffer.Free(fb_pool_get());

        if (!s->handler->error(e, s->handler_ctx))
            return;

        GError *error = new_error_errno_msg2(e, "recv error");
        s->fd = -1;
        istream_deinit_abort(&s->output, error);
    }
}

static void
socket_try_read(struct istream_socket *s)
{
    if (istream_check_direct(&s->output, s->fd_type))
        socket_try_direct(s);
    else
        socket_try_buffered(s);
}

/*
 * istream implementation
 *
 */

static inline struct istream_socket *
istream_to_socket(struct istream *istream)
{
    return &ContainerCast2(*istream, &istream_socket::output);
}

static off_t
istream_socket_available(struct istream *istream, bool partial)
{
    struct istream_socket *s = istream_to_socket(istream);

    assert(socket_valid(s));

    if (s->buffer.IsNull() || (!partial && s->fd >= 0))
        return -1;

    return s->buffer.GetAvailable();
}

static void
istream_socket_read(struct istream *istream)
{
    struct istream_socket *s = istream_to_socket(istream);

    assert(socket_valid(s));

    socket_try_read(s);
}

static void
istream_socket_close(struct istream *istream)
{
    struct istream_socket *s = istream_to_socket(istream);

    assert(socket_valid(s));

    if (s->buffer.IsDefined())
        s->buffer.Free(fb_pool_get());

    p_event_del(&s->event, s->output.pool);
    s->fd = -1;

    s->handler->close(s->handler_ctx);

    istream_deinit(&s->output);
}

static const struct istream_class istream_socket = {
    .available = istream_socket_available,
    .read = istream_socket_read,
    .close = istream_socket_close,
};

/*
 * libevent callback
 *
 */

static void
socket_event_callback(int fd gcc_unused, short event gcc_unused,
                      void *ctx)
{
    struct istream_socket *s = (struct istream_socket *)ctx;

    assert(fd == s->fd);

    socket_try_read(s);

    pool_commit();
}

/*
 * constructor
 *
 */

struct istream *
istream_socket_new(struct pool *pool, int fd, enum istream_direct fd_type,
                   const struct istream_socket_handler *handler, void *ctx)
{
    assert(fd >= 0);
    assert(handler != nullptr);
    assert(handler->close != nullptr);
    assert(handler->error != nullptr);
    assert(handler->depleted != nullptr);
    assert(handler->finished != nullptr);

    struct istream_socket *s = istream_new_macro(pool, socket);
    s->fd = fd;
    s->fd_type = fd_type;
    s->handler = handler;
    s->handler_ctx = ctx;

    s->buffer.SetNull();

    event_set(&s->event, fd, EV_READ, socket_event_callback, s);
    socket_schedule_read(s);

    return &s->output;
}
