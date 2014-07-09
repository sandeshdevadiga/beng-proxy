/*
 * Caching HTTP responses.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http_cache_internal.hxx"
#include "strmap.hxx"

http_cache_document::http_cache_document(struct pool &pool,
                                         const struct http_cache_info &_info,
                                         struct strmap *request_headers,
                                         http_status_t _status,
                                         struct strmap *response_headers)
    :info(pool, _info),
     vary(_info.vary != nullptr
          ? http_cache_copy_vary(&pool, _info.vary, request_headers)
          : nullptr),
     status(_status),
     headers(response_headers != nullptr
             ? strmap_dup(&pool, response_headers, 7)
             : nullptr)
{
    assert(http_status_is_valid(_status));
}
