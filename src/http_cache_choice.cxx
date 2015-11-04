/*
 * Caching HTTP responses.  Memcached choice backend.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http_cache_choice.hxx"
#include "http_cache_rfc.hxx"
#include "http_cache_info.hxx"
#include "http_cache_internal.hxx"
#include "memcached/memcached_stock.hxx"
#include "memcached/memcached_client.hxx"
#include "tpool.hxx"
#include "format.h"
#include "strmap.hxx"
#include "serialize.hxx"
#include "growing_buffer.hxx"
#include "uset.h"
#include "istream/sink_buffer.hxx"
#include "istream/istream.hxx"
#include "istream/istream_memory.hxx"
#include "pool.hxx"
#include "util/djbhash.h"
#include "util/ConstBuffer.hxx"
#include "util/WritableBuffer.hxx"
#include "util/ByteOrder.hxx"

#include <inline/compiler.h>

#include <stdio.h>

enum {
    CHOICE_MAGIC = 4,
};

struct HttpCacheChoice {
    struct pool *pool;

    struct memcached_stock *stock;

    const char *uri;
    const char *key;

    const struct strmap *request_headers;

    ConstBuffer<void> data;

    struct memcached_set_extras extras;

    union {
        http_cache_choice_get_t get;
        http_cache_choice_commit_t commit;
        http_cache_choice_filter_t filter;
        http_cache_choice_delete_t delete_;
    } callback;

    void *callback_ctx;

    struct async_operation_ref *async_ref;
};

bool
HttpCacheChoiceInfo::VaryFits(const struct strmap *headers) const
{
    return http_cache_vary_fits(vary, headers);
}

/**
 * Calculate a aggregated hash value of the specified string map.
 * This is used as a suffix for the memcached
 */
static unsigned
mcd_vary_hash(const struct strmap *vary)
{
    unsigned hash = 0;

    if (vary == nullptr)
        return 0;

    for (const auto &i : *vary)
        hash ^= djb_hash_string(i.key) ^ djb_hash_string(i.value);

    return hash;
}

/**
 * Auto-abbreviate the input string by replacing a long trailer with
 * its MD5 sum.  This is a hack to allow storing long URIs as a
 * memcached key (250 bytes max).
 */
static const char *
maybe_abbreviate(const char *p)
{
    size_t length = strlen(p);
    if (length < 232)
        return p;

    static char buffer[256];
    char *checksum = g_compute_checksum_for_string(G_CHECKSUM_MD5, p + 200,
                                                   length - 200);
    snprintf(buffer, sizeof(buffer), "%.*s~%s", 200, p, checksum);
    g_free(checksum);
    return buffer;
}

const char *
http_cache_choice_vary_key(struct pool &pool, const char *uri,
                           const struct strmap *vary)
{
    char hash[9];
    format_uint32_hex_fixed(hash, mcd_vary_hash(vary));
    hash[8] = 0;

    uri = maybe_abbreviate(uri);

    return p_strcat(&pool, uri, " ", hash, nullptr);
}

static const char *
http_cache_choice_key(struct pool &pool, const char *uri)
{
    return p_strcat(&pool, maybe_abbreviate(uri), " choice", nullptr);
}

static void
http_cache_choice_buffer_done(void *data0, size_t length, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;
    time_t now = time(nullptr);
    uint32_t magic;
    const char *uri = nullptr;
    bool unclean = false;
    struct uset uset;
    unsigned hash;

    ConstBuffer<void> data(data0, length);
    uset_init(&uset);

    while (!data.IsEmpty()) {
        magic = deserialize_uint32(data);
        if (magic != CHOICE_MAGIC)
            break;

        const time_t expires = deserialize_uint64(data);

        const AutoRewindPool auto_rewind(*tpool);

        const struct strmap *const vary = deserialize_strmap(data, tpool);

        if (data.IsNull()) {
            /* deserialization failure */
            unclean = true;
            break;
        }

        hash = mcd_vary_hash(vary);
        if (hash != 0) {
            if (uset_contains_or_add(&uset, hash))
                /* duplicate: mark the record as
                   "unclean", queue the garbage collector */
                unclean = true;
        }

        if (expires != -1 && expires < now)
            unclean = true;
        else if (uri == nullptr &&
                 http_cache_vary_fits(vary, choice->request_headers))
            uri = http_cache_choice_vary_key(*choice->pool, choice->uri, vary);

        if (uri != nullptr && unclean)
            /* we have already found something, and we think that this
               record is unclean - no point in parsing more, abort
               here */
            break;
    }

    choice->callback.get(uri, unclean, nullptr, choice->callback_ctx);
}

static void
http_cache_choice_buffer_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.get(nullptr, true, error, choice->callback_ctx);
}

static const struct sink_buffer_handler http_cache_choice_buffer_handler = {
    .done = http_cache_choice_buffer_done,
    .error = http_cache_choice_buffer_error,
};

static void
http_cache_choice_get_response(enum memcached_response_status status,
                               gcc_unused const void *extras,
                               gcc_unused size_t extras_length,
                               gcc_unused const void *key,
                               gcc_unused size_t key_length,
                               Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (status != MEMCACHED_STATUS_NO_ERROR || value == nullptr) {
        if (value != nullptr)
            value->CloseUnused();

        choice->callback.get(nullptr, false, nullptr, choice->callback_ctx);
        return;
    }

    sink_buffer_new(*choice->pool, *value,
                    http_cache_choice_buffer_handler, choice,
                    *choice->async_ref);
}

static void
http_cache_choice_get_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.get(nullptr, false, error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_get_handler = {
    .response = http_cache_choice_get_response,
    .error = http_cache_choice_get_error,
};

void
http_cache_choice_get(struct pool &pool, struct memcached_stock &stock,
                      const char *uri, const struct strmap *request_headers,
                      http_cache_choice_get_t callback,
                      void *callback_ctx,
                      struct async_operation_ref &async_ref)
{
    auto choice = PoolAlloc<HttpCacheChoice>(pool);

    choice->pool = &pool;
    choice->stock = &stock;
    choice->uri = uri;
    choice->key = http_cache_choice_key(pool, uri);
    choice->request_headers = request_headers;
    choice->callback.get = callback;
    choice->callback_ctx = callback_ctx;
    choice->async_ref = &async_ref;

    memcached_stock_invoke(&pool, &stock,
                           MEMCACHED_OPCODE_GET,
                           nullptr, 0,
                           choice->key, strlen(choice->key),
                           nullptr,
                           &http_cache_choice_get_handler, choice,
                           &async_ref);
}

HttpCacheChoice *
http_cache_choice_prepare(struct pool &pool, const char *uri,
                          const HttpCacheResponseInfo &info,
                          const struct strmap &vary)
{
    auto choice = PoolAlloc<HttpCacheChoice>(pool);

    choice->pool = &pool;
    choice->uri = uri;

    GrowingBuffer *gb = growing_buffer_new(tpool, 1024);
    serialize_uint32(gb, CHOICE_MAGIC);
    serialize_uint64(gb, info.expires);
    serialize_strmap(gb, vary);

    auto data = growing_buffer_dup(gb, &pool);
    choice->data = { data.data, data.size };

    return choice;
}

static void
http_cache_choice_add_response(gcc_unused enum memcached_response_status status,
                               gcc_unused const void *extras,
                               gcc_unused size_t extras_length,
                               gcc_unused const void *key,
                               gcc_unused size_t key_length,
                               Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (value != nullptr)
        value->CloseUnused();

    choice->callback.commit(nullptr, choice->callback_ctx);
}

static void
http_cache_choice_add_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.commit(error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_add_handler = {
    .response = http_cache_choice_add_response,
    .error = http_cache_choice_add_error,
};

static void
http_cache_choice_prepend_response(enum memcached_response_status status,
                                   gcc_unused const void *extras,
                                   gcc_unused size_t extras_length,
                                   gcc_unused const void *key,
                                   gcc_unused size_t key_length,
                                   Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (value != nullptr)
        value->CloseUnused();

    switch (status) {
    case MEMCACHED_STATUS_ITEM_NOT_STORED:
        /* could not prepend: try to add a new record */

        cache_log(5, "add '%s'\n", choice->key);

        choice->extras.flags = 0;
        choice->extras.expiration = ToBE32(600); /* XXX */

        value = istream_memory_new(choice->pool,
                                   choice->data.data, choice->data.size);
        memcached_stock_invoke(choice->pool, choice->stock,
                               MEMCACHED_OPCODE_ADD,
                               &choice->extras, sizeof(choice->extras),
                               choice->key, strlen(choice->key),
                               value,
                               &http_cache_choice_add_handler, choice,
                               choice->async_ref);
        break;

    case MEMCACHED_STATUS_NO_ERROR:
    default:
        choice->callback.commit(nullptr, choice->callback_ctx);
        break;
    }
}

static void
http_cache_choice_prepend_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.commit(error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_prepend_handler = {
    .response = http_cache_choice_prepend_response,
    .error = http_cache_choice_prepend_error,
};

void
http_cache_choice_commit(HttpCacheChoice &choice,
                         struct memcached_stock &stock,
                         http_cache_choice_commit_t callback,
                         void *callback_ctx,
                         struct async_operation_ref &async_ref)
{
    choice.key = http_cache_choice_key(*choice.pool, choice.uri);
    choice.stock = &stock;
    choice.callback.commit = callback;
    choice.callback_ctx = callback_ctx;
    choice.async_ref = &async_ref;

    cache_log(5, "prepend '%s'\n", choice.key);

    Istream *value = istream_memory_new(choice.pool,
                                        choice.data.data,
                                        choice.data.size);
    memcached_stock_invoke(choice.pool, &stock,
                           MEMCACHED_OPCODE_PREPEND,
                           nullptr, 0,
                           choice.key, strlen(choice.key), value,
                           &http_cache_choice_prepend_handler, &choice,
                           &async_ref);
}

static void
http_cache_choice_filter_set_response(gcc_unused enum memcached_response_status status,
                                      gcc_unused const void *extras,
                                      gcc_unused size_t extras_length,
                                      gcc_unused const void *key,
                                      gcc_unused size_t key_length,
                                      gcc_unused Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (value != nullptr)
        value->CloseUnused();

    choice->callback.filter(nullptr, nullptr, choice->callback_ctx);
}

static void
http_cache_choice_filter_set_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.filter(nullptr, error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_filter_set_handler = {
    .response = http_cache_choice_filter_set_response,
    .error = http_cache_choice_filter_set_error,
};

static void
http_cache_choice_filter_buffer_done(void *data0, size_t length, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    ConstBuffer<void> data(data0, length);
    char *dest = (char *)data0;

    while (!data.IsEmpty()) {
        const void *current = data.data;

        const uint32_t magic = deserialize_uint32(data);
        if (magic != CHOICE_MAGIC)
            break;

        HttpCacheChoiceInfo info;
        info.expires = deserialize_uint64(data);

        const AutoRewindPool auto_rewind(*tpool);
        info.vary = deserialize_strmap(data, tpool);

        if (data.IsNull())
            /* deserialization failure */
            break;

        if (choice->callback.filter(&info, nullptr, choice->callback_ctx)) {
            memmove(dest, current, (const uint8_t *)data.data + data.size - (const uint8_t *)current);
            dest += (const uint8_t *)data.data - (const uint8_t *)current;
        }
    }

    if (dest - length == data0)
        /* no change */
        choice->callback.filter(nullptr, nullptr, choice->callback_ctx);
    else if (dest == data0)
        /* no entries left */
        /* XXX use CAS */
        memcached_stock_invoke(choice->pool, choice->stock,
                               MEMCACHED_OPCODE_DELETE,
                               nullptr, 0,
                               choice->key, strlen(choice->key),
                               nullptr,
                               &http_cache_choice_filter_set_handler, choice,
                               choice->async_ref);
    else {
        /* send new contents */
        /* XXX use CAS */

        choice->extras.flags = 0;
        choice->extras.expiration = ToBE32(600); /* XXX */

        memcached_stock_invoke(choice->pool, choice->stock,
                               MEMCACHED_OPCODE_REPLACE,
                               &choice->extras, sizeof(choice->extras),
                               choice->key, strlen(choice->key),
                               istream_memory_new(choice->pool, data0,
                                                  dest - (char *)data0),
                               &http_cache_choice_filter_set_handler, choice,
                               choice->async_ref);
    }
}

static void
http_cache_choice_filter_buffer_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.filter(nullptr, error, choice->callback_ctx);
}

static const struct sink_buffer_handler http_cache_choice_filter_buffer_handler = {
    .done = http_cache_choice_filter_buffer_done,
    .error = http_cache_choice_filter_buffer_error,
};

static void
http_cache_choice_filter_get_response(enum memcached_response_status status,
                                      gcc_unused const void *extras,
                                      gcc_unused size_t extras_length,
                                      gcc_unused const void *key,
                                      gcc_unused size_t key_length,
                                      Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (status != MEMCACHED_STATUS_NO_ERROR || value == nullptr) {
        if (value != nullptr)
            value->CloseUnused();

        choice->callback.filter(nullptr, nullptr, choice->callback_ctx);
        return;
    }

    sink_buffer_new(*choice->pool, *value,
                    http_cache_choice_filter_buffer_handler, choice,
                    *choice->async_ref);
}

static void
http_cache_choice_filter_get_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.filter(nullptr, error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_filter_get_handler = {
    .response = http_cache_choice_filter_get_response,
    .error = http_cache_choice_filter_get_error,
};

void
http_cache_choice_filter(struct pool &pool, struct memcached_stock &stock,
                         const char *uri,
                         http_cache_choice_filter_t callback,
                         void *callback_ctx,
                         struct async_operation_ref &async_ref)
{
    auto choice = PoolAlloc<HttpCacheChoice>(pool);

    choice->pool = &pool;
    choice->stock = &stock;
    choice->uri = uri;
    choice->key = http_cache_choice_key(pool, uri);
    choice->callback.filter = callback;
    choice->callback_ctx = callback_ctx;
    choice->async_ref = &async_ref;

    memcached_stock_invoke(&pool, &stock,
                           MEMCACHED_OPCODE_GET,
                           nullptr, 0,
                           choice->key, strlen(choice->key),
                           nullptr,
                           &http_cache_choice_filter_get_handler, choice,
                           &async_ref);
}

struct cleanup_data {
    time_t now;
    struct uset uset;

    http_cache_choice_cleanup_t callback;
    void *callback_ctx;
};

static bool
http_cache_choice_cleanup_filter_callback(const HttpCacheChoiceInfo *info,
                                          GError *error, void *ctx)
{
    cleanup_data *data = (cleanup_data *)ctx;

    if (info != nullptr) {
        unsigned hash = mcd_vary_hash(info->vary);
        bool duplicate = uset_contains_or_add(&data->uset, hash);
        return (info->expires == -1 ||
                info->expires >= data->now) &&
            !duplicate;
    } else {
        data->callback(error, data->callback_ctx);
        return false;
    }
}

void
http_cache_choice_cleanup(struct pool &pool, struct memcached_stock &stock,
                          const char *uri,
                          http_cache_choice_cleanup_t callback,
                          void *callback_ctx,
                          struct async_operation_ref &async_ref)
{
    auto data = NewFromPool<cleanup_data>(pool);

    data->now = time(nullptr);
    uset_init(&data->uset);
    data->callback = callback;
    data->callback_ctx = callback_ctx;

    http_cache_choice_filter(pool, stock, uri,
                             http_cache_choice_cleanup_filter_callback, data,
                             async_ref);
}

static void
http_cache_choice_delete_response(gcc_unused enum memcached_response_status status,
                                  gcc_unused const void *extras,
                                  gcc_unused size_t extras_length,
                                  gcc_unused const void *key,
                                  gcc_unused size_t key_length,
                                  Istream *value, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    if (value != nullptr)
        value->CloseUnused();

    choice->callback.delete_(nullptr, choice->callback_ctx);
}

static void
http_cache_choice_delete_error(GError *error, void *ctx)
{
    auto choice = (HttpCacheChoice *)ctx;

    choice->callback.delete_(error, choice->callback_ctx);
}

static const struct memcached_client_handler http_cache_choice_delete_handler = {
    .response = http_cache_choice_delete_response,
    .error = http_cache_choice_delete_error,
};

void
http_cache_choice_delete(struct pool &pool, struct memcached_stock &stock,
                         const char *uri,
                         http_cache_choice_delete_t callback,
                         void *callback_ctx,
                         struct async_operation_ref &async_ref)
{
    auto choice = PoolAlloc<HttpCacheChoice>(pool);

    choice->pool = &pool;
    choice->stock = &stock;
    choice->uri = uri;
    choice->key = http_cache_choice_key(pool, uri);
    choice->callback.delete_ = callback;
    choice->callback_ctx = callback_ctx;
    choice->async_ref = &async_ref;

    memcached_stock_invoke(&pool, &stock,
                           MEMCACHED_OPCODE_GET,
                           nullptr, 0,
                           choice->key, strlen(choice->key),
                           nullptr,
                           &http_cache_choice_delete_handler, choice,
                           &async_ref);
}
