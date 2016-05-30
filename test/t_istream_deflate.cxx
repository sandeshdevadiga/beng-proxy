#include "istream/istream_deflate.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"

class EventLoop;

static Istream *
create_input(struct pool *pool)
{
    return istream_string_new(pool, "foo");
}

static Istream *
create_test(EventLoop &event_loop, struct pool *pool, Istream *input)
{
    return istream_deflate_new(*pool, *input, event_loop);
}

#include "t_istream_filter.hxx"
