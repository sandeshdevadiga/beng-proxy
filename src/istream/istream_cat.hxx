/*
 * Concatenate several istreams.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ISTREAM_CAT_HXX
#define BENG_PROXY_ISTREAM_CAT_HXX

struct pool;
class Istream;

Istream *
istream_cat_new(struct pool &pool, ...);

#endif
