#include "istream/istream_hold.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"

#define EXPECTED_RESULT "foo"

static struct istream *
create_input(struct pool *pool)
{
    return istream_string_new(pool, "foo");
}

static struct istream *
create_test(struct pool *pool, struct istream *input)
{
    return istream_hold_new(pool, input);
}

#include "t_istream_filter.hxx"
