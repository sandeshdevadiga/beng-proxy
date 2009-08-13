/*
 * A auto-growing buffer you can write to.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "growing-buffer.h"
#include "istream-internal.h"

#include <assert.h>
#include <string.h>

struct buffer {
    struct buffer *next;
    size_t length, position;
    char data[sizeof(size_t)];
};

struct growing_buffer {
    pool_t pool;
    struct istream stream;
    size_t size;
    struct buffer *current, *tail, first;
};

struct growing_buffer *__attr_malloc
growing_buffer_new(pool_t pool, size_t initial_size)
{
    struct growing_buffer *gb = p_malloc(pool, sizeof(*gb) - sizeof(gb->first.data) + initial_size);

    gb->pool = pool;
    gb->size = initial_size;
    gb->current = &gb->first;
    gb->tail = &gb->first;
    gb->first.next = NULL;
    gb->first.length = 0;
    gb->first.position = 0;

    return gb;
}

void *
growing_buffer_write(struct growing_buffer *gb, size_t length)
{
    struct buffer *buffer = gb->tail;
    void *ret;

    assert(gb->size > 0);

    if (buffer->length + length > gb->size) {
        if (gb->size < length)
            gb->size = length; /* XXX round up? */
        buffer = p_malloc(gb->pool, sizeof(*buffer) - sizeof(buffer->data) + gb->size);
        buffer->next = NULL;
        buffer->length = 0;
        buffer->position = 0;
        gb->tail->next = buffer;
        gb->tail = buffer;
    }

    assert(buffer->length + length <= gb->size);

    ret = buffer->data + buffer->length;
    buffer->length += length;

    return ret;
}

void
growing_buffer_write_buffer(struct growing_buffer *gb, const void *p, size_t length)
{
    memcpy(growing_buffer_write(gb, length), p, length);
}

void
growing_buffer_write_string(struct growing_buffer *gb, const char *p)
{
    growing_buffer_write_buffer(gb, p, strlen(p));
}

bool
growing_buffer_empty(struct growing_buffer *gb)
{
    assert(gb->current != NULL);

    while (gb->current->position == gb->current->length) {
        assert(gb->current->position <= gb->current->length);
        if (gb->current->next == NULL)
            return true;
        gb->current = gb->current->next;
        assert(gb->current->position == 0);
    }

    return false;
}

size_t
growing_buffer_size(const struct growing_buffer *gb)
{
    size_t size = 0;

    for (const struct buffer *buffer = &gb->first;
         buffer != NULL; buffer = buffer->next)
        size += buffer->length;

    return size;
}

const void *
growing_buffer_read(struct growing_buffer *gb, size_t *length_r)
{
    assert(gb->current != NULL);

    while (gb->current->position == gb->current->length) {
        assert(gb->current->position <= gb->current->length);
        if (gb->current->next == NULL)
            return NULL;
        gb->current = gb->current->next;
        assert(gb->current->position == 0);
    }

    assert(gb->current->position < gb->current->length);

    *length_r = gb->current->length - gb->current->position;
    return gb->current->data + gb->current->position;
}

void
growing_buffer_consume(struct growing_buffer *gb, size_t length)
{
    assert(gb->current != NULL);
    assert(gb->current->position <= gb->current->length);

    while (length > 0) {
        assert(gb->current != NULL);

        if (gb->current->position + length < gb->current->length) {
            gb->current->position += length;
            length = 0;
        } else {
            length -= gb->current->length - gb->current->position;
            gb->current->position = gb->current->length;

            if (length > 0) {
                assert(gb->current->next != NULL);
                gb->current = gb->current->next;
            }
        }
    }

    if (gb->current->next == NULL &&
        gb->current->position == gb->current->length) {
        /* allow buffer recycling - we're on the last buffer, and
           someone might still be writing to it.  if we clear it here,
           the full buffer may be reused */
        gb->current->position = 0;
        gb->current->length = 0;
    }
}

size_t
growing_buffer_length(const struct growing_buffer *gb)
{
    size_t length = 0;

    for (const struct buffer *buffer = &gb->first; buffer != NULL;
         buffer = buffer->next)
        length += buffer->length;

    return length;
}

static void *
growing_buffer_copy(void *dest0, const struct growing_buffer *gb)
{
    unsigned char *dest = dest0;

    for (const struct buffer *buffer = &gb->first; buffer != NULL;
         buffer = buffer->next) {
        memcpy(dest, buffer->data, buffer->length);
        dest += buffer->length;
    }

    return dest;
}

void *
growing_buffer_dup(const struct growing_buffer *gb, pool_t pool,
                   size_t *length_r)
{
    unsigned char *dest;
    size_t length;

    length = growing_buffer_length(gb);
    *length_r = length;
    if (length == 0)
        return NULL;

    dest = p_malloc(pool, length);
    growing_buffer_copy(dest, gb);

    return dest;
}

void *
growing_buffer_dup2(const struct growing_buffer *a,
                    const struct growing_buffer *b,
                    pool_t pool, size_t *length_r)
{
    void *dest;
    size_t length;

    length = growing_buffer_length(a) + growing_buffer_length(b);
    *length_r = length;
    if (length == 0)
        return NULL;

    dest = p_malloc(pool, length);
    growing_buffer_copy(growing_buffer_copy(dest, a), b);

    return dest;
}


static inline struct growing_buffer *
istream_to_gb(istream_t istream)
{
    return (struct growing_buffer *)(((char*)istream) - offsetof(struct growing_buffer, stream));
}

static off_t
istream_gb_available(istream_t istream, bool partial __attr_unused)
{
    struct growing_buffer *gb = istream_to_gb(istream);
    struct buffer *buffer;
    off_t available = 0;

    assert(gb->size == 0);
    assert(gb->tail == NULL);
    assert(gb->current != NULL);
    assert(gb->current->position <= gb->current->length);

    for (buffer = gb->current; buffer != NULL; buffer = buffer->next)
        available += buffer->length - buffer->position;

    return available;
}

static void
istream_gb_read(istream_t istream)
{
    struct growing_buffer *gb = istream_to_gb(istream);
    const void *data;
    size_t length, nbytes;

    assert(gb->size == 0);
    assert(gb->tail == NULL);
    assert(gb->current != NULL);
    assert(gb->current->position <= gb->current->length);

    /* this loop is required to cross the buffer borders */
    while (1) {
        data = growing_buffer_read(gb, &length);
        if (data == NULL) {
            gb->current = NULL;
            istream_invoke_eof(&gb->stream);
            return;
        }

        nbytes = istream_invoke_data(&gb->stream, data, length);
        if (nbytes == 0 && gb->current == NULL)
            /* growing_buffer has been closed */
            return;

        growing_buffer_consume(gb, nbytes);
        if (nbytes < length)
            return;
    }
}

static void
istream_gb_close(istream_t istream)
{
    struct growing_buffer *gb = istream_to_gb(istream);

    assert(gb->size == 0);
    assert(gb->tail == NULL);

    gb->current = NULL;

    istream_invoke_abort(&gb->stream);
}

static const struct istream istream_gb = {
    .available = istream_gb_available,
    .read = istream_gb_read,
    .close = istream_gb_close,
};

istream_t
growing_buffer_istream(struct growing_buffer *gb)
{
    assert(gb->size > 0);

    gb->size = 0; /* "read mode" marker for assertions */
    assert(gb->first.position == 0);
    assert(gb->current == &gb->first);
    gb->tail = NULL;
    gb->stream = istream_gb;
    gb->stream.pool = gb->pool;

    return istream_struct_cast(&gb->stream);
}
