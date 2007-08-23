/*
 * Handler for HTTP requests.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "instance.h"
#include "http-server.h"
#include "connection.h"
#include "handler.h"

#include <assert.h>

void
remove_connection(struct client_connection *connection)
{
    assert(connection != NULL);
    assert(connection->http != NULL);

    list_remove(&connection->siblings);

    http_server_connection_free(&connection->http);

    pool_unref(connection->pool);
}

void
http_listener_callback(int fd,
                       const struct sockaddr *addr, socklen_t addrlen,
                       void *ctx)
{
    struct instance *instance = (struct instance*)ctx;
    pool_t pool;
    struct client_connection *connection;

    (void)addr;
    (void)addrlen;
    (void)ctx;

    pool = pool_new_linear(instance->pool, "client_connection", 16384);
    connection = p_malloc(pool, sizeof(*connection));
    connection->pool = pool;

    list_add(&connection->siblings, &instance->connections);

    connection->http = http_server_connection_new(pool, fd,
                                                  my_http_server_callback, connection);
    pool_ref(connection->pool);
    http_server_try_read(connection->http);
    pool_unref(connection->pool);
}
