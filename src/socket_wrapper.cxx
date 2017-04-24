/*
 * Wrapper for a socket file descriptor.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "socket_wrapper.hxx"
#include "io/Buffered.hxx"
#include "io/Splice.hxx"

#include <socket/util.h>

#include <unistd.h>
#include <sys/socket.h>

void
SocketWrapper::ReadEventCallback(unsigned events)
{
    assert(IsValid());

    if (events & EV_TIMEOUT)
        handler.OnSocketTimeout();
    else
        handler.OnSocketRead();
}

void
SocketWrapper::WriteEventCallback(unsigned events)
{
    assert(IsValid());

    if (events & EV_TIMEOUT)
        handler.OnSocketTimeout();
    else
        handler.OnSocketWrite();
}

void
SocketWrapper::Init(int _fd, FdType _fd_type)
{
    assert(_fd >= 0);

    fd = SocketDescriptor::FromFileDescriptor(FileDescriptor(_fd));
    fd_type = _fd_type;

    read_event.Set(fd.Get(), EV_READ|EV_PERSIST);
    write_event.Set(fd.Get(), EV_WRITE|EV_PERSIST);
}

void
SocketWrapper::Init(SocketWrapper &&src)
{
    Init(src.fd.Get(), src.fd_type);
    src.Abandon();
}

void
SocketWrapper::Shutdown()
{
    if (!fd.IsDefined())
        return;

    shutdown(fd.Get(), SHUT_RDWR);
}

void
SocketWrapper::Close()
{
    if (!fd.IsDefined())
        return;

    read_event.Delete();
    write_event.Delete();

    fd.Close();
}

void
SocketWrapper::Abandon()
{
    assert(fd.IsDefined());

    read_event.Delete();
    write_event.Delete();

    fd = SocketDescriptor::Undefined();
}

int
SocketWrapper::AsFD()
{
    assert(IsValid());

    const int result = fd.Get();
    Abandon();
    return result;
}

ssize_t
SocketWrapper::ReadToBuffer(ForeignFifoBuffer<uint8_t> &buffer, size_t length)
{
    assert(IsValid());

    return recv_to_buffer(fd.Get(), buffer, length);
}

void
SocketWrapper::SetCork(bool cork)
{
    assert(IsValid());

    socket_set_cork(fd.Get(), cork);
}

bool
SocketWrapper::IsReadyForWriting() const
{
    assert(IsValid());

    return fd.IsReadyForWriting();
}

ssize_t
SocketWrapper::Write(const void *data, size_t length)
{
    assert(IsValid());

    return send(fd.Get(), data, length, MSG_DONTWAIT|MSG_NOSIGNAL);
}

ssize_t
SocketWrapper::WriteV(const struct iovec *v, size_t n)
{
    assert(IsValid());

    struct msghdr m = {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = const_cast<struct iovec *>(v),
        .msg_iovlen = n,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    return sendmsg(fd.Get(), &m, MSG_DONTWAIT|MSG_NOSIGNAL);
}

ssize_t
SocketWrapper::WriteFrom(int other_fd, FdType other_fd_type,
                         size_t length)
{
    return SpliceToSocket(other_fd_type, other_fd, fd.Get(), length);
}

