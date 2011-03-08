/*
 * Store a URI along with a list of socket addresses.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "uri-address.h"
#include "uri-edit.h"

#include <socket/address.h>

#include <string.h>

struct uri_address {
    struct list_head siblings;

    socklen_t length;

    struct sockaddr address;
};

struct uri_with_address *
uri_address_new(pool_t pool, const char *uri)
{
    struct uri_with_address *uwa = p_malloc(pool, sizeof(*uwa));
    uwa->pool = pool;
    uwa->uri = p_strdup(pool, uri);
    address_list_init(&uwa->addresses);

    return uwa;
}

struct uri_with_address *
uri_address_dup(pool_t pool, const struct uri_with_address *uwa)
{
    struct uri_with_address *p = p_malloc(pool, sizeof(*uwa));

    p->pool = pool;
    p->uri = p_strdup(pool, uwa->uri);

    address_list_copy(pool, &p->addresses, &uwa->addresses);

    return p;
}

struct uri_with_address *
uri_address_insert_query_string(pool_t pool,
                                const struct uri_with_address *uwa,
                                const char *query_string)
{
    struct uri_with_address *p = p_malloc(pool, sizeof(*uwa));

    p->pool = pool;
    p->uri = uri_insert_query_string(pool, uwa->uri, query_string);

    address_list_copy(pool, &p->addresses, &uwa->addresses);

    return p;
}

void
uri_address_add(struct uri_with_address *uwa,
                const struct sockaddr *addr, socklen_t addrlen)
{
    address_list_add(uwa->pool, &uwa->addresses, addr, addrlen);
}

const struct sockaddr *
uri_address_first(const struct uri_with_address *uwa, socklen_t *addrlen_r)
{
    return address_list_first(&uwa->addresses, addrlen_r);
}

const struct sockaddr *
uri_address_next(struct uri_with_address *uwa, socklen_t *addrlen_r)
{
    return address_list_next(&uwa->addresses, addrlen_r);
}

bool
uri_address_is_single(const struct uri_with_address *uwa)
{
    return address_list_is_single(&uwa->addresses);
}

const char *
uri_address_key(const struct uri_with_address *uwa)
{
    return address_list_key(&uwa->addresses);
}
