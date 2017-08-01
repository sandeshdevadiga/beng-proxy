/*
 * Launch and manage "Local HTTP" child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lhttp_stock.hxx"
#include "lhttp_address.hxx"
#include "stock/Stock.hxx"
#include "stock/MapStock.hxx"
#include "stock/MultiStock.hxx"
#include "stock/Class.hxx"
#include "stock/Item.hxx"
#include "lease.hxx"
#include "child_stock.hxx"
#include "spawn/JailParams.hxx"
#include "spawn/Prepared.hxx"
#include "event/SocketEvent.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "io/Logger.hxx"
#include "util/RuntimeError.hxx"
#include "util/Exception.hxx"

#include <assert.h>
#include <unistd.h>
#include <string.h>

class LhttpStock {
    StockMap hstock;
    StockMap *const child_stock;
    MultiStock *const mchild_stock;

public:
    LhttpStock(unsigned limit, unsigned max_idle,
               EventLoop &event_loop, SpawnService &spawn_service);

    ~LhttpStock() {
        /* call FadeAll() release all idle connections before calling
           mstock_free() to avoid assertion failure */
        hstock.FadeAll();

        mstock_free(mchild_stock);
        child_stock_free(child_stock);
    }

    void FadeAll() {
        hstock.FadeAll();
        child_stock->FadeAll();
    }

    StockMap &GetConnectionStock() {
        return hstock;
    }

    MultiStock &GetChildStock() {
        return *mchild_stock;
    }
};

class LhttpConnection final : LoggerDomainFactory, HeapStockItem {
    LazyDomainLogger logger;

    StockItem *child = nullptr;

    struct lease_ref lease_ref;

    UniqueSocketDescriptor fd;
    SocketEvent event;

public:
    explicit LhttpConnection(CreateStockItem c)
        :HeapStockItem(c),
         logger(*this),
         event(c.stock.GetEventLoop(), BIND_THIS_METHOD(EventCallback)) {}

    ~LhttpConnection() override;

    void Connect(MultiStock &child_stock, struct pool &caller_pool,
                 const char *key, void *info,
                 unsigned concurrency);

    SocketDescriptor GetSocket() const {
        assert(fd.IsDefined());
        return fd;
    }

private:
    void EventCallback(unsigned events);

    /* virtual methods from LoggerDomainFactory */
    std::string MakeLoggerDomain() const noexcept override {
        return GetStockName();
    }

    /* virtual methods from class StockItem */
    bool Borrow(gcc_unused void *ctx) override {
        event.Delete();
        return true;
    }

    bool Release(gcc_unused void *ctx) override {
        event.Add(EventDuration<300>::value);
        return true;
    }
};

void
LhttpConnection::Connect(MultiStock &child_stock, struct pool &caller_pool,
                         const char *key, void *info,
                         unsigned concurrency)
{
    try {
        child = mstock_get_now(child_stock,
                               caller_pool,
                               key, info, concurrency,
                               lease_ref);
    } catch (...) {
        delete this;
        std::throw_with_nested(FormatRuntimeError("Failed to launch LHTTP server '%s'",
                                                  key));
    }

    try {
        fd = child_stock_item_connect(child);
    } catch (...) {
        delete this;
        std::throw_with_nested(FormatRuntimeError("Failed to connect to LHTTP server '%s'",
                                                  key));
    }

    event.Set(fd.Get(), SocketEvent::READ);
    InvokeCreateSuccess();
}

static const char *
lhttp_stock_key(struct pool *pool, const LhttpAddress *address)
{
    return address->GetServerId(pool);
}

/*
 * libevent callback
 *
 */

inline void
LhttpConnection::EventCallback(unsigned events)
{
    if ((events & SocketEvent::TIMEOUT) == 0) {
        char buffer;
        ssize_t nbytes = fd.Read(&buffer, sizeof(buffer));
        if (nbytes < 0)
            logger(2, "error on idle LHTTP connection: ", strerror(errno));
        else if (nbytes > 0)
            logger(2, "unexpected data from idle LHTTP connection");
    }

    InvokeIdleDisconnect();
}

/*
 * child_stock class
 *
 */

static int
lhttp_child_stock_socket_type(void *info)
{
    const auto &address = *(const LhttpAddress *)info;

    int type = SOCK_STREAM;
    if (!address.blocking)
        type |= SOCK_NONBLOCK;

    return type;
}

static void
lhttp_child_stock_prepare(void *info, UniqueSocketDescriptor &&fd,
                          PreparedChildProcess &p)
{
    const auto &address = *(const LhttpAddress *)info;

    p.SetStdin(std::move(fd));
    address.CopyTo(p);
}

static const ChildStockClass lhttp_child_stock_class = {
    .socket_type = lhttp_child_stock_socket_type,
    .prepare = lhttp_child_stock_prepare,
};

/*
 * stock class
 *
 */

static void
lhttp_stock_create(void *ctx, CreateStockItem c, void *info,
                   struct pool &caller_pool,
                   gcc_unused CancellablePointer &cancel_ptr)
{
    auto lhttp_stock = (LhttpStock *)ctx;
    const auto *address = (const LhttpAddress *)info;

    assert(address != nullptr);
    assert(address->path != nullptr);

    auto *connection = new LhttpConnection(c);

    connection->Connect(lhttp_stock->GetChildStock(), caller_pool,
                        c.GetStockName(), info, address->concurrency);
}

LhttpConnection::~LhttpConnection()
{
    if (fd.IsDefined()) {
        event.Delete();
        fd.Close();
    }

    if (child != nullptr)
        lease_ref.Release(true);
}

static constexpr StockClass lhttp_stock_class = {
    .create = lhttp_stock_create,
};


/*
 * interface
 *
 */

inline
LhttpStock::LhttpStock(unsigned limit, unsigned max_idle,
                       EventLoop &event_loop, SpawnService &spawn_service)
    :hstock(event_loop, lhttp_stock_class, this, limit, max_idle),
     child_stock(child_stock_new(limit, max_idle,
                                 event_loop, spawn_service,
                                 &lhttp_child_stock_class)),
     mchild_stock(mstock_new(*child_stock)) {}

LhttpStock *
lhttp_stock_new(unsigned limit, unsigned max_idle,
                EventLoop &event_loop, SpawnService &spawn_service)
{
    return new LhttpStock(limit, max_idle, event_loop, spawn_service);
}

void
lhttp_stock_free(LhttpStock *ls)
{
    delete ls;
}

void
lhttp_stock_fade_all(LhttpStock &ls)
{
    ls.FadeAll();
}

StockItem *
lhttp_stock_get(LhttpStock *lhttp_stock, struct pool *pool,
                const LhttpAddress *address)
{
    const auto *const jail = address->options.jail;
    if (jail != nullptr && jail->enabled && jail->home_directory == nullptr)
        throw std::runtime_error("No home directory for jailed LHTTP");

    union {
        const LhttpAddress *in;
        void *out;
    } deconst = { .in = address };

    return lhttp_stock->GetConnectionStock().GetNow(*pool,
                                                    lhttp_stock_key(pool, address),
                                                    deconst.out);
}

SocketDescriptor
lhttp_stock_item_get_socket(const StockItem &item)
{
    const auto *connection = (const LhttpConnection *)&item;

    return connection->GetSocket();
}

FdType
lhttp_stock_item_get_type(gcc_unused const StockItem &item)
{
    return FdType::FD_SOCKET;
}
