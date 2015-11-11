#include "client_balancer.hxx"
#include "net/ConnectSocket.hxx"
#include "net/SocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "pool.hxx"
#include "async.hxx"
#include "balancer.hxx"
#include "failure.hxx"
#include "address_list.hxx"

#include <socket/resolver.h>
#include <socket/util.h>

#include <glib.h>
#include <event.h>

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

struct Context final : ConnectSocketHandler {
    struct balancer *balancer;

    enum {
        NONE, SUCCESS, TIMEOUT, ERROR,
    } result = TIMEOUT;

    SocketDescriptor fd;
    GError *error;

    /* virtual methods from class ConnectSocketHandler */
    void OnSocketConnectSuccess(SocketDescriptor &&new_fd) override {
        result = SUCCESS;
        fd = std::move(new_fd);
        balancer_free(balancer);
    }

    void OnSocketConnectTimeout() override {
        result = TIMEOUT;
        balancer_free(balancer);
    }

    void OnSocketConnectError(GError *_error) override {
        result = ERROR;
        error = _error;
        balancer_free(balancer);
    }
};

/*
 * main
 *
 */

int
main(int argc, char **argv)
{
    if (argc <= 1) {
        fprintf(stderr, "Usage: run-client-balancer ADDRESS ...\n");
        return EXIT_FAILURE;
    }

    /* initialize */

    struct event_base *event_base = event_init();

    struct pool *root_pool = pool_new_libc(nullptr, "root");
    struct pool *pool = pool_new_linear(root_pool, "test", 8192);

    failure_init();

    Context ctx;
    ctx.balancer = balancer_new(*pool);

    AddressList address_list;
    address_list.Init();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    for (int i = 1; i < argc; ++i) {
        const char *p = argv[i];

        struct addrinfo *ai;
        int ret = socket_resolve_host_port(p, 80, &hints, &ai);
        if (ret != 0) {
            fprintf(stderr, "Failed to resolve '%s': %s\n",
                    p, gai_strerror(ret));
            return EXIT_FAILURE;
        }

        for (struct addrinfo *j = ai; j != nullptr; j = j->ai_next)
            address_list.Add(pool, {ai->ai_addr, ai->ai_addrlen});

        freeaddrinfo(ai);
    }

    /* connect */

    struct async_operation_ref async_ref;
    client_balancer_connect(pool, ctx.balancer,
                            false, SocketAddress::Null(),
                            0, &address_list, 30,
                            ctx, &async_ref);

    event_dispatch();

    assert(ctx.result != Context::NONE);

    /* cleanup */

    failure_deinit();

    pool_unref(pool);
    pool_commit();

    pool_unref(root_pool);
    pool_commit();
    pool_recycler_clear();

    event_base_free(event_base);

    switch (ctx.result) {
    case Context::NONE:
        break;

    case Context::SUCCESS:
        return EXIT_SUCCESS;

    case Context::TIMEOUT:
        fprintf(stderr, "timeout\n");
        return EXIT_FAILURE;

    case Context::ERROR:
        fprintf(stderr, "%s\n", ctx.error->message);
        g_error_free(ctx.error);
        return EXIT_FAILURE;
    }

    assert(false);
    return EXIT_FAILURE;
}
