/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "http_cache_rfc.hxx"
#include "http_cache_document.hxx"
#include "http_cache_internal.hxx"
#include "http_address.hxx"
#include "http_util.hxx"
#include "strmap.hxx"
#include "ResourceAddress.hxx"
#include "cgi_address.hxx"
#include "lhttp_address.hxx"
#include "pool.hxx"
#include "io/Logger.hxx"
#include "http/Date.hxx"
#include "util/StringView.hxx"

#include <assert.h>
#include <stdlib.h>

static StringView
next_item(StringView &s)
{
    s.StripLeft();
    if (s.IsEmpty())
        return nullptr;

    StringView result;

    const char *comma = s.Find(',');
    if (comma == nullptr) {
        result = s;
        s.SetEmpty();
    } else {
        result = {s.data, comma};
        s.MoveFront(comma + 1);
    }

    result.StripRight();
    return result;
}

/* check whether the request could produce a cacheable response */
bool
http_cache_request_evaluate(HttpCacheRequestInfo &info,
                            http_method_t method,
                            const ResourceAddress &address,
                            const StringMap &headers,
                            Istream *body)
{
    if (method != HTTP_METHOD_GET || body != nullptr)
        /* RFC 2616 13.11 "Write-Through Mandatory" */
        return false;

    const char *p = headers.Get("range");
    if (p != nullptr)
        return false;

    /* RFC 2616 14.8: "When a shared cache receives a request
       containing an Authorization field, it MUST NOT return the
       corresponding response as a reply to any other request
       [...] */
    if (headers.Get("authorization") != nullptr)
        return false;

    p = headers.Get("cache-control");
    if (p != nullptr) {
        StringView cc = p, s;

        while (!(s = next_item(cc)).IsNull()) {
            if (s.Equals("no-cache") || s.Equals("no-store"))
                return false;

            if (s.Equals("only-if-cached"))
                info.only_if_cached = true;
        }
    } else {
        p = headers.Get("pragma");
        if (p != nullptr && strcmp(p, "no-cache") == 0)
            return false;
    }

    info.is_remote = address.type == ResourceAddress::Type::HTTP;
    info.has_query_string = address.HasQueryString();

    return true;
}

gcc_pure
bool
http_cache_vary_fits(const StringMap &vary, const StringMap *headers)
{
    for (const auto &i : vary) {
        const char *value = strmap_get_checked(headers, i.key);
        if (value == nullptr)
            value = "";

        if (strcmp(i.value, value) != 0)
            /* mismatch in one of the "Vary" request headers */
            return false;
    }

    return true;
}

gcc_pure
bool
http_cache_vary_fits(const StringMap *vary, const StringMap *headers)
{
    return vary == nullptr || http_cache_vary_fits(*vary, headers);
}

bool
http_cache_request_invalidate(http_method_t method)
{
    /* RFC 2616 13.10 "Invalidation After Updates or Deletions" */
    return method == HTTP_METHOD_PUT || method == HTTP_METHOD_DELETE ||
        method == HTTP_METHOD_POST;
}

static std::chrono::system_clock::time_point
parse_translate_time(const char *p, std::chrono::system_clock::duration offset)
{
    if (p == nullptr)
        return std::chrono::system_clock::from_time_t(-1);

    auto t = http_date_parse(p);
    if (t != std::chrono::system_clock::from_time_t(-1))
        t += offset;

    return t;
}

/**
 * RFC 2616 13.4
 */
static bool
http_status_cacheable(http_status_t status)
{
    return status == HTTP_STATUS_OK ||
        status == HTTP_STATUS_NON_AUTHORITATIVE_INFORMATION ||
        status == HTTP_STATUS_PARTIAL_CONTENT ||
        status == HTTP_STATUS_MULTIPLE_CHOICES ||
        status == HTTP_STATUS_MOVED_PERMANENTLY ||
        status == HTTP_STATUS_GONE;
}

gcc_pure
static const char *
strmap_get_non_empty(const StringMap &map, const char *key)
{
    const char *value = map.Get(key);
    if (value != nullptr && *value == 0)
        value = nullptr;
    return value;
}

bool
http_cache_response_evaluate(const HttpCacheRequestInfo &request_info,
                             HttpCacheResponseInfo &info,
                             http_status_t status, const StringMap &headers,
                             off_t body_available)
{
    const char *p;

    if (!http_status_cacheable(status))
        return false;

    if (body_available != (off_t)-1 && body_available > cacheable_size_limit)
        /* too large for the cache */
        return false;

    info.expires = std::chrono::system_clock::from_time_t(-1);
    p = headers.Get("cache-control");
    if (p != nullptr) {
        StringView cc = p, s;

        while (!(s = next_item(cc)).IsNull()) {
            if (s.StartsWith("private") ||
                s.Equals("no-cache") || s.Equals("no-store"))
                return false;

            if (s.StartsWith({"max-age=", 8})) {
                /* RFC 2616 14.9.3 */
                char value[16];
                int seconds;

                StringView param(s.data + 8, s.size - 8);
                if (param.size >= sizeof(value))
                    continue;

                memcpy(value, param.data, param.size);
                value[param.size] = 0;

                seconds = atoi(value);
                if (seconds > 0)
                    info.expires = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
            }
        }
    }

    const auto now = std::chrono::system_clock::now();

    std::chrono::system_clock::duration offset;
    if (request_info.is_remote) {
        p = headers.Get("date");
        if (p == nullptr)
            /* we cannot determine whether to cache a resource if the
               server does not provide its system time */
            return false;

        auto date = http_date_parse(p);
        if (date == std::chrono::system_clock::from_time_t(-1))
            return false;

        offset = now - date;
    } else
        offset = std::chrono::system_clock::duration::zero();


    if (info.expires == std::chrono::system_clock::from_time_t(-1)) {
        /* RFC 2616 14.9.3: "If a response includes both an Expires
           header and a max-age directive, the max-age directive
           overrides the Expires header" */

        info.expires = parse_translate_time(headers.Get("expires"), offset);
        if (info.expires != std::chrono::system_clock::from_time_t(-1) &&
            info.expires < now)
            LogConcat(4, "HttpCache", "invalid 'expires' header");
    }

    if (request_info.has_query_string &&
        info.expires == std::chrono::system_clock::from_time_t(-1))
        /* RFC 2616 13.9: "since some applications have traditionally
           used GETs and HEADs with query URLs (those containing a "?"
           in the rel_path part) to perform operations with
           significant side effects, caches MUST NOT treat responses
           to such URIs as fresh unless the server provides an
           explicit expiration time" */
        return false;

    info.last_modified = headers.Get("last-modified");
    info.etag = headers.Get("etag");

    info.vary = strmap_get_non_empty(headers, "vary");
    if (info.vary != nullptr && strcmp(info.vary, "*") == 0)
        /* RFC 2616 13.6: A Vary header field-value of "*" always
           fails to match and subsequent requests on that resource can
           only be properly interpreted by the origin server. */
        return false;

    return info.expires != std::chrono::system_clock::from_time_t(-1) ||
        info.last_modified != nullptr ||
        info.etag != nullptr;
}

void
http_cache_copy_vary(StringMap &dest, struct pool &pool, const char *vary,
                     const StringMap &request_headers)
{
    for (char **list = http_list_split(pool, vary);
         *list != nullptr; ++list) {
        const char *name = *list;
        const char *value = request_headers.Get(name);
        if (value == nullptr)
            value = "";
        else
            value = p_strdup(&pool, value);
        dest.Set(name, value);
    }
}

bool
http_cache_prefer_cached(const HttpCacheDocument &document,
                         const StringMap &response_headers)
{
    if (document.info.etag == nullptr)
        return false;

    const char *etag = response_headers.Get("etag");

    /* if the ETags are the same, then the resource hasn't changed,
       but the server was too lazy to check that properly */
    return etag != nullptr && strcmp(etag, document.info.etag) == 0;
}
