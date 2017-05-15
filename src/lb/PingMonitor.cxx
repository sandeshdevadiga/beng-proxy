/*
 * Ping (ICMP) monitor class.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "PingMonitor.hxx"
#include "Monitor.hxx"
#include "pool.hxx"
#include "net/Ping.hxx"
#include "net/SocketAddress.hxx"
#include "util/Cancellable.hxx"

class LbPingClientHandler final : public PingClientHandler {
    LbMonitorHandler &handler;

public:
    explicit LbPingClientHandler(LbMonitorHandler &_handler)
        :handler(_handler) {}

    void PingResponse() override {
        handler.Success();
    }

    void PingTimeout() override {
        handler.Timeout();
    }

    void PingError(std::exception_ptr ep) override {
        handler.Error(ep);
    }
};

static void
ping_monitor_run(EventLoop &event_loop, struct pool &pool,
                 gcc_unused const LbMonitorConfig &config,
                 SocketAddress address,
                 LbMonitorHandler &handler,
                 CancellablePointer &cancel_ptr)
{
    ping(event_loop, pool, address,
         *NewFromPool<LbPingClientHandler>(pool, handler),
         cancel_ptr);
}

const LbMonitorClass ping_monitor_class = {
    .run = ping_monitor_run,
};