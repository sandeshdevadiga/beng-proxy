#include "istream/sink_header.hxx"
#include "istream/istream.hxx"
#include "istream/istream_delayed.hxx"
#include "istream/istream_hold.hxx"
#include "istream/istream_memory.hxx"
#include "async.hxx"

#include <string.h>

#define EXPECTED_RESULT "foo"

static Istream *
create_input(struct pool *pool)
{
    return istream_memory_new(pool, "\0\0\0\x06" "foobarfoo", 13);
}

static void
my_sink_header_done(void *header, size_t length, Istream &tail,
                    void *ctx)
{
    Istream *delayed = (Istream *)ctx;

    assert(length == 6);
    assert(header != NULL);
    assert(memcmp(header, "foobar", 6) == 0);

    istream_delayed_set(*delayed, tail);
    if (delayed->HasHandler())
        delayed->Read();
}

static void
my_sink_header_error(GError *error, void *ctx)
{
    Istream *delayed = (Istream *)ctx;

    istream_delayed_set_abort(*delayed, error);
}

static const struct sink_header_handler my_sink_header_handler = {
    .done = my_sink_header_done,
    .error = my_sink_header_error,
};

static Istream *
create_test(struct pool *pool, Istream *input)
{
    Istream *delayed, *hold;

    delayed = istream_delayed_new(pool);
    hold = istream_hold_new(*pool, *delayed);

    sink_header_new(*pool, *input,
                    my_sink_header_handler, delayed,
                    *istream_delayed_async_ref(*delayed));

    input->Read();

    return hold;
}

#define NO_BLOCKING
#define NO_GOT_DATA_ASSERT

#include "t_istream_filter.hxx"
