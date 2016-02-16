/*
 * Launch and manage WAS child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_WAS_STOCK_HXX
#define BENG_PROXY_WAS_STOCK_HXX

#include <inline/compiler.h>

struct pool;
struct StockMap;
struct StockItem;
class StockGetHandler;
struct ChildOptions;
struct async_operation_ref;
struct WasProcess;
template<typename T> struct ConstBuffer;
class SpawnService;

StockMap *
was_stock_new(unsigned limit, unsigned max_idle,
              SpawnService &spawn_service);

/**
 * @param args command-line arguments
 */
void
was_stock_get(StockMap *hstock, struct pool *pool,
              const ChildOptions &options,
              const char *executable_path,
              ConstBuffer<const char *> args,
              StockGetHandler &handler,
              struct async_operation_ref &async_ref);

/**
 * Returns the socket descriptor of the specified stock item.
 */
gcc_pure
const WasProcess &
was_stock_item_get(const StockItem &item);

#endif
