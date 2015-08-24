/*
 * Launch and manage FastCGI child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "fcgi_stock.hxx"
#include "fcgi_quark.h"
#include "fcgi_launch.hxx"
#include "hstock.hxx"
#include "stock.hxx"
#include "child_stock.hxx"
#include "child_manager.hxx"
#include "ChildOptions.hxx"
#include "pevent.hxx"
#include "gerrno.h"
#include "pool.hxx"
#include "JailConfig.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Cast.hxx"

#include <daemon/log.h>

#include <glib.h>

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifdef __linux
#include <sched.h>
#endif

struct FcgiStock {
    StockMap *hstock;
    StockMap *child_stock;

    void FadeAll() {
        hstock_fade_all(*hstock);
        hstock_fade_all(*child_stock);
    }
};

struct FcgiChildParams {
    const char *executable_path;

    ConstBuffer<const char *> args;
    ConstBuffer<const char *> env;

    const ChildOptions *options;

    const char *GetStockKey(struct pool &pool) const;
};

struct FcgiConnection {
    StockItem base;

    JailParams jail_params;

    struct jail_config jail_config;

    StockItem *child;

    int fd;
    struct event event;

    /**
     * Is this a fresh connection to the FastCGI child process?
     */
    bool fresh;

    /**
     * Shall the FastCGI child process be killed?
     */
    bool kill;

    /**
     * Was the current request aborted by the fcgi_client caller?
     */
    bool aborted;

    gcc_pure
    const char *GetStockKey() const {
        return child_stock_item_key(child);
    }
};

const char *
FcgiChildParams::GetStockKey(struct pool &pool) const
{
    const char *key = executable_path;

    for (auto i : args)
        key = p_strcat(&pool, key, " ", i, nullptr);

    for (auto i : env)
        key = p_strcat(&pool, key, "$", i, nullptr);

    char options_buffer[4096];
    *options->MakeId(options_buffer) = 0;
    if (*options_buffer != 0)
        key = p_strcat(&pool, key, options_buffer, nullptr);

    return key;
}

/*
 * libevent callback
 *
 */

static void
fcgi_connection_event_callback(int fd, gcc_unused short event, void *ctx)
{
    auto *connection = (FcgiConnection *)ctx;

    assert(fd == connection->fd);

    p_event_consumed(&connection->event, connection->base.pool);

    if ((event & EV_TIMEOUT) == 0) {
        char buffer;
        ssize_t nbytes = recv(fd, &buffer, sizeof(buffer), MSG_DONTWAIT);
        if (nbytes < 0)
            daemon_log(2, "error on idle FastCGI connection '%s': %s\n",
                       connection->GetStockKey(), strerror(errno));
        else if (nbytes > 0)
            daemon_log(2, "unexpected data from idle FastCGI connection '%s'\n",
                       connection->GetStockKey());
    }

    stock_del(connection->base);
    pool_commit();
}

/*
 * child_stock class
 *
 */

static int
fcgi_child_stock_clone_flags(gcc_unused const char *key, void *info, int flags,
                             gcc_unused void *ctx)
{
    const FcgiChildParams *params =
        (const FcgiChildParams *)info;
    const ChildOptions *const options = params->options;

    return options->ns.GetCloneFlags(flags);
}

static int
fcgi_child_stock_run(gcc_unused struct pool *pool, gcc_unused const char *key,
                     void *info, gcc_unused void *ctx)
{
    const FcgiChildParams *params =
        (const FcgiChildParams *)info;
    const ChildOptions *const options = params->options;

    options->Apply(true);

    fcgi_run(&options->jail, params->executable_path,
             params->args, params->env);
}

static const struct child_stock_class fcgi_child_stock_class = {
    .shutdown_signal = SIGUSR1,
    .clone_flags = fcgi_child_stock_clone_flags,
    .run = fcgi_child_stock_run,
};

/*
 * stock class
 *
 */

static constexpr FcgiConnection &
ToFcgiConnection(StockItem &item)
{
    return ContainerCast2(item, &FcgiConnection::base);
}

static constexpr const FcgiConnection &
ToFcgiConnection(const StockItem &item)
{
    return ContainerCast2(item, &FcgiConnection::base);
}

static struct pool *
fcgi_stock_pool(void *ctx gcc_unused, struct pool &parent,
               const char *uri gcc_unused)
{
    return pool_new_linear(&parent, "fcgi_connection", 2048);
}

static void
fcgi_stock_create(void *ctx, StockItem &item,
                  const char *key, void *info,
                  gcc_unused struct pool &caller_pool,
                  gcc_unused struct async_operation_ref &async_ref)
{
    FcgiStock *fcgi_stock = (FcgiStock *)ctx;
    struct pool *pool = item.pool;
    FcgiChildParams *params = (FcgiChildParams *)info;
    auto *connection = &ToFcgiConnection(item);

    assert(key != nullptr);
    assert(params != nullptr);
    assert(params->executable_path != nullptr);

    const ChildOptions *const options = params->options;
    if (options->jail.enabled) {
        connection->jail_params.CopyFrom(*pool, options->jail);

        if (!jail_config_load(&connection->jail_config,
                              "/etc/cm4all/jailcgi/jail.conf", pool)) {
            GError *error = g_error_new(fcgi_quark(), 0,
                                        "Failed to load /etc/cm4all/jailcgi/jail.conf");
            stock_item_failed(item, error);
            return;
        }
    } else
        connection->jail_params.enabled = false;

    GError *error = nullptr;
    connection->child = hstock_get_now(*fcgi_stock->child_stock, *pool,
                                       key, params, &error);
    if (connection->child == nullptr) {
        g_prefix_error(&error, "failed to start to FastCGI server '%s': ",
                       key);

        stock_item_failed(item, error);
        return;
    }

    connection->fd = child_stock_item_connect(connection->child, &error);
    if (connection->fd < 0) {
        g_prefix_error(&error, "failed to connect to FastCGI server '%s': ",
                       key);

        child_stock_put(fcgi_stock->child_stock, connection->child, true);
        stock_item_failed(item, error);
        return;
    }

    connection->fresh = true;
    connection->kill = false;

    event_set(&connection->event, connection->fd, EV_READ|EV_TIMEOUT,
              fcgi_connection_event_callback, connection);

    stock_item_available(connection->base);
}

static bool
fcgi_stock_borrow(void *ctx gcc_unused, StockItem &item)
{
    auto *connection = &ToFcgiConnection(item);

    /* check the connection status before using it, just in case the
       FastCGI server has decided to close the connection before
       fcgi_connection_event_callback() got invoked */
    char buffer;
    ssize_t nbytes = recv(connection->fd, &buffer, sizeof(buffer),
                          MSG_DONTWAIT);
    if (nbytes > 0) {
        daemon_log(2, "unexpected data from idle FastCGI connection '%s'\n",
                   connection->GetStockKey());
        return false;
    } else if (nbytes == 0) {
        /* connection closed (not worth a log message) */
        return false;
    } else if (errno != EAGAIN) {
        daemon_log(2, "error on idle FastCGI connection '%s': %s\n",
                   connection->GetStockKey(), strerror(errno));
        return false;
    }

    p_event_del(&connection->event, connection->base.pool);
    connection->aborted = false;
    return true;
}

static void
fcgi_stock_release(void *ctx gcc_unused, StockItem &item)
{
    auto *connection = &ToFcgiConnection(item);
    static const struct timeval tv = {
        .tv_sec = 300,
        .tv_usec = 0,
    };

    connection->fresh = false;

    p_event_add(&connection->event, &tv, connection->base.pool,
                "fcgi_connection_event");
}

static void
fcgi_stock_destroy(void *ctx, StockItem &item)
{
    FcgiStock *fcgi_stock = (FcgiStock *)ctx;
    auto *connection = &ToFcgiConnection(item);

    p_event_del(&connection->event, connection->base.pool);
    close(connection->fd);

    child_stock_put(fcgi_stock->child_stock, connection->child,
                    connection->kill);
}

static constexpr StockClass fcgi_stock_class = {
    .item_size = sizeof(FcgiConnection),
    .pool = fcgi_stock_pool,
    .create = fcgi_stock_create,
    .borrow = fcgi_stock_borrow,
    .release = fcgi_stock_release,
    .destroy = fcgi_stock_destroy,
};


/*
 * interface
 *
 */

FcgiStock *
fcgi_stock_new(struct pool *pool, unsigned limit, unsigned max_idle)
{
    auto fcgi_stock = NewFromPool<FcgiStock>(*pool);
    fcgi_stock->child_stock = child_stock_new(pool, limit, max_idle,
                                              &fcgi_child_stock_class);
    fcgi_stock->hstock = hstock_new(*pool, fcgi_stock_class, fcgi_stock,
                                    limit, max_idle);

    return fcgi_stock;
}

void
fcgi_stock_free(FcgiStock *fcgi_stock)
{
    hstock_free(fcgi_stock->hstock);
    hstock_free(fcgi_stock->child_stock);
}

void
fcgi_stock_fade_all(FcgiStock &fs)
{
    fs.FadeAll();
}

StockItem *
fcgi_stock_get(FcgiStock *fcgi_stock, struct pool *pool,
               const ChildOptions &options,
               const char *executable_path,
               ConstBuffer<const char *> args,
               ConstBuffer<const char *> env,
               GError **error_r)
{
    auto params = NewFromPool<FcgiChildParams>(*pool);
    params->executable_path = executable_path;
    params->args = args;
    params->env = env;
    params->options = &options;

    return hstock_get_now(*fcgi_stock->hstock, *pool,
                          params->GetStockKey(*pool), params,
                          error_r);
}

int
fcgi_stock_item_get_domain(gcc_unused const StockItem &item)
{
    return AF_UNIX;
}

int
fcgi_stock_item_get(const StockItem &item)
{
    const auto *connection = &ToFcgiConnection(item);

    assert(connection->fd >= 0);

    return connection->fd;
}

const char *
fcgi_stock_translate_path(const StockItem &item,
                          const char *path, struct pool *pool)
{
    const auto *connection = &ToFcgiConnection(item);

    if (!connection->jail_params.enabled)
        /* no JailCGI - application's namespace is the same as ours,
           no translation needed */
        return path;

    const char *jailed = jail_translate_path(&connection->jail_config, path,
                                             connection->jail_params.home_directory,
                                             pool);
    return jailed != nullptr ? jailed : path;
}

void
fcgi_stock_put(FcgiStock *fcgi_stock, StockItem &item,
               bool destroy)
{
    auto *connection = &ToFcgiConnection(item);

    if (connection->fresh && connection->aborted && destroy)
        /* the fcgi_client caller has aborted the request before the
           first response on a fresh connection was received: better
           kill the child process, it may be failing on us
           completely */
        connection->kill = true;

    hstock_put(*fcgi_stock->hstock, connection->GetStockKey(),
               item, destroy);
}

void
fcgi_stock_aborted(StockItem &item)
{
    auto *connection = &ToFcgiConnection(item);

    connection->aborted = true;
}
