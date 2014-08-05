/*
 * Utilities for buffered I/O.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "buffered_io.hxx"
#include "fifo_buffer.hxx"

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

ssize_t
read_to_buffer(int fd, struct fifo_buffer *buffer, size_t length)
{
    assert(fd >= 0);
    assert(buffer != nullptr);

    size_t max_length;
    void *dest = fifo_buffer_write(buffer, &max_length);
    if (dest == nullptr)
        return -2;

    if (length > max_length)
        length = max_length;

    ssize_t nbytes = read(fd, dest, length);
    if (nbytes > 0)
        fifo_buffer_append(buffer, (size_t)nbytes);

    return nbytes;
}

ssize_t
write_from_buffer(int fd, struct fifo_buffer *buffer)
{
    size_t length;
    const void *data = fifo_buffer_read(buffer, &length);
    if (data == nullptr)
        return -2;

    ssize_t nbytes = write(fd, data, length);
    if (nbytes < 0 && errno != EAGAIN)
        return -1;

    if (nbytes <= 0)
        return length;

    fifo_buffer_consume(buffer, (size_t)nbytes);
    return (ssize_t)length - nbytes;
}

ssize_t
recv_to_buffer(int fd, struct fifo_buffer *buffer, size_t length)
{
    assert(fd >= 0);
    assert(buffer != nullptr);

    size_t max_length;
    void *dest = fifo_buffer_write(buffer, &max_length);
    if (dest == nullptr)
        return -2;

    if (length > max_length)
        length = max_length;

    ssize_t nbytes = recv(fd, dest, length, MSG_DONTWAIT);
    if (nbytes > 0)
        fifo_buffer_append(buffer, (size_t)nbytes);

    return nbytes;
}

ssize_t
send_from_buffer(int fd, struct fifo_buffer *buffer)
{
    size_t length;
    const void *data = fifo_buffer_read(buffer, &length);
    if (data == nullptr)
        return -2;

    ssize_t nbytes = send(fd, data, length, MSG_DONTWAIT|MSG_NOSIGNAL);
    if (nbytes < 0 && errno != EAGAIN)
        return -1;

    if (nbytes <= 0)
        return length;

    fifo_buffer_consume(buffer, (size_t)nbytes);
    return (ssize_t)length - nbytes;
}
