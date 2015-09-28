/*
 * This istream filter reads a 32 bit header size from the stream,
 * reads it into a buffer and invokes a callback with the tail of the
 * stream.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "sink_header.hxx"
#include "ForwardIstream.hxx"
#include "async.hxx"
#include "util/Cast.hxx"
#include "util/ByteOrder.hxx"

#include <glib.h>

#include <assert.h>
#include <string.h>
#include <stdint.h>

class HeaderSink final : public ForwardIstream {
    enum {
        SIZE, HEADER, CALLBACK, DATA
    } state = SIZE;

    unsigned char size_buffer[4];

    unsigned char *buffer;
    size_t size, position = 0;

    /**
     * How much data of the input is pending to be consumed?  Only
     * valid while state==CALLBACK.
     */
    size_t pending;

    const struct sink_header_handler *handler;
    void *handler_ctx;

    struct async_operation operation;

public:
    HeaderSink(struct pool &_pool, struct istream &_input,
               const struct sink_header_handler &_handler, void *_ctx,
               struct async_operation_ref &async_ref)
        :ForwardIstream(_pool, _input,
                        MakeIstreamHandler<HeaderSink>::handler, this),
         handler(&_handler), handler_ctx(_ctx) {
        operation.Init2<HeaderSink, &HeaderSink::operation>();
        async_ref.Set(operation);
    }

    void Abort() {
        input.Close();
        Destroy();
    }

    size_t InvokeCallback(size_t consumed);

    size_t ConsumeSize(const void *data, size_t length);
    size_t ConsumeHeader(const void *data, size_t length);

    /* virtual methods from class Istream */

    off_t GetAvailable(bool partial) override;

    void Read() override {
        if (state == HeaderSink::CALLBACK)
            /* workaround: when invoking the callback from the data()
               handler, it would be illegal to call header->input again */
            return;

        ForwardIstream::Read();
    }

    /* handler */

    size_t OnData(const void *data, size_t length);
    ssize_t OnDirect(FdType type, int fd, size_t max_length);
    void OnEof();
    void OnError(GError *error);
};

static GQuark
sink_header_quark(void)
{
    return g_quark_from_static_string("sink_header");
}

size_t
HeaderSink::InvokeCallback(size_t consumed)
{
    assert(state == SIZE || state == HEADER);

    operation.Finished();

    const ScopePoolRef ref(GetPool() TRACE_ARGS);

    /* the base value has been set by sink_header_input_data() */
    pending += consumed;

    state = CALLBACK;
    handler->done(buffer, size,
                  Cast(),
                  handler_ctx);

    if (input.IsDefined()) {
        state = DATA;
        input.SetDirect(GetHandlerDirect());
    } else
        /* we have been closed meanwhile; bail out */
        consumed = 0;

    return consumed;
}

inline size_t
HeaderSink::ConsumeSize(const void *data, size_t length)
{
    assert(position < sizeof(size_buffer));

    if (length > sizeof(size_buffer) - position)
        length = sizeof(size_buffer) - position;

    memcpy(size_buffer + position, data, length);
    position += length;

    if (position < sizeof(size_buffer))
        return length;

    const uint32_t *size_p = (const uint32_t *)(const void *)size_buffer;
    size = FromBE32(*size_p);
    if (size > 0x100000) {
        /* header too large */
        operation.Finished();
        input.Close();

        GError *error =
            g_error_new_literal(sink_header_quark(), 0,
                                "header is too large");
        handler->error(error, handler_ctx);
        Destroy();
        return 0;
    }

    if (size > 0) {
        buffer = (unsigned char *)p_malloc(&GetPool(), size);
        state = HeaderSink::HEADER;
        position = 0;
    } else {
        /* header empty: don't allocate, invoke callback now */

        buffer = nullptr;

        length = InvokeCallback(length);
    }

    return length;
}

inline size_t
HeaderSink::ConsumeHeader(const void *data, size_t length)
{
    size_t nbytes = size - position;

    assert(position < size);

    if (nbytes > length)
        nbytes = length;

    memcpy(buffer + position, data, nbytes);
    position += nbytes;

    if (position < size)
        return nbytes;

    return InvokeCallback(nbytes);
}


/*
 * istream handler
 *
 */

inline size_t
HeaderSink::OnData(const void *data0, size_t length)
{
    const unsigned char *data = (const unsigned char *)data0;
    size_t consumed = 0, nbytes;

    if (state == DATA)
        return InvokeData(data, length);

    if (state == SIZE) {
        pending = 0; /* just in case the callback is invoked */

        consumed = ConsumeSize(data, length);
        if (consumed == 0)
            return 0;

        if (consumed == length)
            return length;

        data += consumed;
        length -= consumed;
    }

    if (state == HEADER) {
        pending = consumed; /* just in case the callback is invoked */

        nbytes = ConsumeHeader(data, length);
        if (nbytes == 0)
            return 0;

        consumed += nbytes;
        if (consumed == length)
            return length;

        data += nbytes;
        length -= nbytes;
    }

    assert(consumed > 0);

    if (state == DATA && length > 0) {
        const ScopePoolRef ref(GetPool() TRACE_ARGS);

        nbytes = InvokeData(data, length);
        if (nbytes == 0 && !input.IsDefined())
            consumed = 0;
        else
            consumed += nbytes;
    }

    return consumed;
}

inline ssize_t
HeaderSink::OnDirect(FdType type, int fd, size_t max_length)
{
    assert(state == DATA);

    return ForwardIstream::OnDirect(type, fd, max_length);
}

inline void
HeaderSink::OnEof()
{
    GError *error;

    switch (state) {
    case SIZE:
    case HEADER:
        operation.Finished();

        error = g_error_new_literal(sink_header_quark(), 0,
                                    "premature end of file");
        handler->error(error, handler_ctx);
        Destroy();
        break;

    case CALLBACK:
        assert(false);
        gcc_unreachable();

    case DATA:
        DestroyEof();
        break;
    }
}

inline void
HeaderSink::OnError(GError *error)
{
    switch (state) {
    case SIZE:
    case HEADER:
        operation.Finished();
        handler->error(error, handler_ctx);
        Destroy();
        break;

    case CALLBACK:
        assert(false);
        gcc_unreachable();

    case DATA:
        DestroyError(error);
        break;
    }
}

/*
 * istream implementation
 *
 */

off_t
HeaderSink::GetAvailable(bool partial)
{
    off_t available = ForwardIstream::GetAvailable(partial);

    if (available >= 0 && state == HeaderSink::CALLBACK) {
        if (available < (off_t)pending) {
            assert(partial);

            return -1;
        }

        available -= pending;
    }

    return available;
}

/*
 * constructor
 *
 */

void
sink_header_new(struct pool *pool, struct istream *input,
                const struct sink_header_handler *handler, void *ctx,
                struct async_operation_ref *async_ref)
{
    assert(input != NULL);
    assert(!istream_has_handler(input));
    assert(handler != NULL);
    assert(handler->done != NULL);
    assert(handler->error != NULL);

    NewIstream<HeaderSink>(*pool, *input, *handler, ctx, *async_ref);

}
