/*
 * Wrapper for a socket file descriptor.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "socket_wrapper.hxx"
#include "direct.h"
#include "buffered_io.h"
#include "fd-util.h"
#include "fd_util.h"
#include "pool.h"
#include "pevent.h"

#include <socket/util.h>

#include <unistd.h>
#include <sys/socket.h>

static void
socket_read_event_callback(gcc_unused int fd, short event, void *ctx)
{
    struct socket_wrapper *s = (struct socket_wrapper *)ctx;
    assert(s->IsValid());

    if (event & EV_TIMEOUT)
        s->handler->timeout(s->handler_ctx);
    else
        s->handler->read(s->handler_ctx);

    pool_commit();
}

static void
socket_write_event_callback(gcc_unused int fd, gcc_unused short event,
                            void *ctx)
{
    struct socket_wrapper *s = (struct socket_wrapper *)ctx;
    assert(s->IsValid());

    if (event & EV_TIMEOUT)
        s->handler->timeout(s->handler_ctx);
    else
        s->handler->write(s->handler_ctx);

    pool_commit();
}

void
socket_wrapper::Init(struct pool *_pool,
                     int _fd, enum istream_direct _fd_type,
                     const struct socket_handler *_handler, void *_ctx)
{
    assert(_pool != nullptr);
    assert(_fd >= 0);
    assert(_handler != nullptr);
    assert(_handler->read != nullptr);
    assert(_handler->write != nullptr);

    pool = _pool;
    fd = _fd;
    fd_type = _fd_type;
    direct_mask = istream_direct_mask_to(fd_type);

    event_set(&read_event, fd, EV_READ|EV_PERSIST|EV_TIMEOUT,
              socket_read_event_callback, this);

    event_set(&write_event, fd, EV_WRITE|EV_PERSIST|EV_TIMEOUT,
              socket_write_event_callback, this);

    handler = _handler;
    handler_ctx = _ctx;
}

void
socket_wrapper::Close()
{
    if (fd < 0)
        return;

    p_event_del(&read_event, pool);
    p_event_del(&write_event, pool);

    close(fd);
    fd = -1;
}

void
socket_wrapper::Abandon()
{
    assert(fd >= 0);

    p_event_del(&read_event, pool);
    p_event_del(&write_event, pool);

    fd = -1;
}

int
socket_wrapper::AsFD()
{
    assert(IsValid());

    const int result = dup_cloexec(fd);
    Abandon();
    return result;
}

ssize_t
socket_wrapper::ReadToBuffer(struct fifo_buffer *buffer, size_t length)
{
    assert(IsValid());

    return recv_to_buffer(fd, buffer, length);
}

void
socket_wrapper::SetCork(bool cork)
{
    assert(IsValid());

    socket_set_cork(fd, cork);
}

bool
socket_wrapper::IsReadyForWriting() const
{
    assert(IsValid());

    return fd_ready_for_writing(fd);
}

ssize_t
socket_wrapper::Write(const void *data, size_t length)
{
    assert(IsValid());

    return send(fd, data, length, MSG_DONTWAIT|MSG_NOSIGNAL);
}

ssize_t
socket_wrapper::WriteFrom(int other_fd, enum istream_direct other_fd_type,
                          size_t length)
{
    return istream_direct_to_socket(other_fd_type, other_fd, fd, length);
}

