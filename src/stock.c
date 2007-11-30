/*
 * Objects in stock.  May be used for connection pooling.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "stock.h"

#include <assert.h>

struct stock {
    pool_t pool;
    const struct stock_class *class;
    void *class_ctx;
    const char *uri;

    unsigned num_idle;
    struct list_head idle;
};

struct stock *
stock_new(pool_t pool, const struct stock_class *class,
          void *class_ctx, const char *uri)
{
    struct stock *stock;

    assert(pool != NULL);
    assert(class != NULL);
    assert(class->item_size > sizeof(struct stock_item));
    assert(class->create != NULL);
    assert(class->validate != NULL);
    assert(class->destroy != NULL);

    pool = pool_new_linear(pool, "stock", 1024);
    stock = p_malloc(pool, sizeof(*stock));
    stock->pool = pool;
    stock->class = class;
    stock->class_ctx = class_ctx;
    stock->uri = uri;
    stock->num_idle = 0;
    list_init(&stock->idle);

    return stock;
}

void
stock_free(struct stock **stock_r)
{
    struct stock *stock;

    assert(stock_r != NULL);
    assert(*stock_r != NULL);

    stock = *stock_r;
    *stock_r = NULL;

    while (stock->num_idle > 0) {
        struct stock_item *item = (struct stock_item *)stock->idle.next;

        assert(!list_empty(&stock->idle));

        list_remove(&item->list_head);
        --stock->num_idle;

        stock->class->destroy(stock->class_ctx, item);
    }

    pool_unref(stock->pool);
}

struct stock_item *
stock_get(struct stock *stock)
{
    struct stock_item *item;
    int ret;

    assert(stock != NULL);

    while (stock->num_idle > 0) {
        assert(!list_empty(&stock->idle));

        item = (struct stock_item *)stock->idle.next;
        list_remove(&item->list_head);
        --stock->num_idle;

        assert(item->is_idle);

        if (stock->class->validate(stock->class_ctx, item)) {
            item->is_idle = 0;
            return item;
        }

        stock->class->destroy(stock->class_ctx, item);
        p_free(stock->pool, item);
    }

    item = p_malloc(stock->pool, stock->class->item_size);
    item->stock = stock;
    item->is_idle = 0;

    ret = stock->class->create(stock->class_ctx, item, stock->uri);
    if (!ret) {
        p_free(stock->pool, item);
        return NULL;
    }

    return item;
}

void
stock_put(struct stock_item *item, int destroy)
{
    struct stock *stock;

    assert(item != NULL);
    assert(!item->is_idle);

    stock = item->stock;

    assert(stock != NULL);
    assert(pool_contains(stock->pool, item, stock->class->item_size));

    if (destroy || stock->num_idle >= 8 ||
        !stock->class->validate(stock->class_ctx, item)) {
        stock->class->destroy(stock->class_ctx, item);
        p_free(stock->pool, item);
    } else {
        item->is_idle = 1;
        list_add(&item->list_head, &stock->idle);
        ++stock->num_idle;
    }
}
