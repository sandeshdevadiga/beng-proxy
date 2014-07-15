/*
 * Glue code for using the ssl_filter in a client connection.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ssl_client.hxx"
#include "ssl_config.hxx"
#include "ssl_factory.hxx"
#include "ssl_filter.hxx"
#include "thread_socket_filter.hxx"
#include "thread_pool.hxx"
#include "util/Error.hxx"

#include <daemon/log.h>

#include <glib.h>

static ssl_factory *factory;

void
ssl_client_init(void)
{
    ssl_config config;

    Error error;
    factory = ssl_factory_new(config, false, error);
    if (factory == nullptr)
        daemon_log(1, "ssl_factory_new() failed: %s\n", error.GetMessage());
}

void
ssl_client_deinit(void)
{
    if (factory != nullptr)
        ssl_factory_free(factory);
}

const struct socket_filter *
ssl_client_get_filter(void)
{
    return &thread_socket_filter;;
}

void *
ssl_client_create(struct pool *pool,
                  const char *hostname,
                  GError **error_r)
{
    (void)hostname; // TODO: use this parameter

    auto ssl = ssl_filter_new(pool, *factory, error_r);
    if (ssl == nullptr)
        return nullptr;

    auto &queue = thread_pool_get_queue();
    return thread_socket_filter_new(*pool, queue,
                                    ssl_thread_socket_filter, ssl);
}
