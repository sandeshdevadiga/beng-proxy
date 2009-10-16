/*
 * Objects in stock.  May be used for connection pooling.
 *
 * The 'stock' class holds a number of idle objects.  The 'hstock'
 * class is a hash table of any number of 'stock' objects, each with a
 * different URI.  The URI may be something like a hostname:port pair
 * for HTTP client connections - it is not used by this class, but
 * passed to the stock_class.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_STOCK_H
#define __BENG_STOCK_H

#include "pool.h"

#include <inline/list.h>

#include <stdbool.h>

struct async_operation_ref;
struct stock_item;

typedef void (*stock_callback_t)(void *ctx, struct stock_item *item);

struct stock_item {
    struct list_head list_head;
    struct stock *stock;
    pool_t pool;

#ifndef NDEBUG
    bool is_idle;
#endif

    stock_callback_t callback;
    void *callback_ctx;
};

struct stock_class {
    size_t item_size;

    pool_t (*pool)(void *ctx, pool_t parent, const char *uri);
    void (*create)(void *ctx, struct stock_item *item,
                   const char *uri, void *info,
                   pool_t caller_pool,
                   struct async_operation_ref *async_ref);
    bool (*borrow)(void *ctx, struct stock_item *item);
    void (*release)(void *ctx, struct stock_item *item);
    void (*destroy)(void *ctx, struct stock_item *item);
};


/* stock.c */

struct stock;

struct stock *
stock_new(pool_t pool, const struct stock_class *class,
          void *class_ctx, const char *uri, unsigned limit);

void
stock_free(struct stock *stock);

/**
 * Returns true if there are no items in the stock - neither idle nor
 * busy.
 */
bool
stock_is_empty(const struct stock *stock);

void
stock_get(struct stock *stock, pool_t pool, void *info,
          stock_callback_t callback, void *callback_ctx,
          struct async_operation_ref *async_ref);

/**
 * Obtains an item from the stock without going through the callback.
 * This requires a stock class which finishes the create() method
 * immediately.
 */
struct stock_item *
stock_get_now(struct stock *stock, pool_t pool, void *info);

void
stock_item_available(struct stock_item *item);

void
stock_item_failed(struct stock_item *item);

void
stock_item_aborted(struct stock_item *item);

void
stock_put(struct stock_item *item, bool destroy);

void
stock_del(struct stock_item *item);


/* hstock.c */

struct hstock;

struct hstock *
hstock_new(pool_t pool, const struct stock_class *class, void *class_ctx,
           unsigned limit);

void
hstock_free(struct hstock *hstock);

void
hstock_get(struct hstock *hstock, pool_t pool,
           const char *uri, void *info,
           stock_callback_t callback, void *callback_ctx,
           struct async_operation_ref *async_ref);

void
hstock_put(struct hstock *hstock, const char *uri, struct stock_item *item,
           bool destroy);


#endif
