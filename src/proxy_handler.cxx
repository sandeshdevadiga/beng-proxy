/*
 * Serve HTTP requests from another HTTP/AJP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.hxx"
#include "bp_connection.hxx"
#include "bp_instance.hxx"
#include "request.hxx"
#include "request_forward.hxx"
#include "ResourceLoader.hxx"
#include "http_server/Request.hxx"
#include "http_response.hxx"
#include "http_address.hxx"
#include "cgi_address.hxx"
#include "cookie_client.hxx"
#include "uri/uri_extract.hxx"
#include "strmap.hxx"
#include "http_response.hxx"
#include "istream/istream_pipe.hxx"
#include "lhttp_address.hxx"
#include "pool.hxx"

/**
 * Return a copy of the URI for forwarding to the next server.  This
 * omits the beng-proxy request "arguments".
 */
gcc_pure
static const char *
ForwardURI(struct pool &pool, const parsed_uri &uri)
{
    if (uri.query.IsEmpty())
        return p_strdup(pool, uri.base);
    else
        return p_strncat(&pool,
                         uri.base.data, uri.base.size,
                         "?", (size_t)1,
                         uri.query.data, uri.query.size,
                         nullptr);
}

/**
 * Return a copy of the original request URI for forwarding to the
 * next server.  This omits the beng-proxy request "arguments" (unless
 * the translation server declared the "transparent" mode).
 */
gcc_pure
static const char *
ForwardURI(const Request &r)
{
    const TranslateResponse &t = *r.translate.response;
    if (t.transparent || r.uri.args.IsEmpty())
        /* transparent or no args: return the full URI as-is */
        return r.request.uri;
    else
        /* remove the "args" part */
        return ForwardURI(r.pool, r.uri);
}

gcc_pure
static const char *
GetCookieHost(const Request &r)
{
    const TranslateResponse &t = *r.translate.response;
    if (t.cookie_host != nullptr)
        return t.cookie_host;

    const ResourceAddress &address = *r.translate.address;
    return address.GetHostAndPort();
}

gcc_pure
static const char *
GetCookieURI(const Request &r)
{
    return r.cookie_uri;
}

static void
proxy_collect_cookies(Request &request2, const StringMap &headers)
{
    auto r = headers.EqualRange("set-cookie2");
    if (r.first == r.second) {
        r = headers.EqualRange("set-cookie");
        if (r.first == r.second)
            return;
    }

    const char *host_and_port = GetCookieHost(request2);
    if (host_and_port == nullptr)
        return;

    const char *path = GetCookieURI(request2);
    if (path == nullptr)
        return;

    auto session = request2.MakeRealmSession();
    if (!session)
        return;

    for (auto i = r.first; i != r.second; ++i)
        cookie_jar_set_cookie2(session->cookies, i->value,
                               host_and_port, path);
}

static void
proxy_response(http_status_t status, StringMap &&headers,
               Istream *body, void *ctx)
{
    auto &request2 = *(Request *)ctx;

#ifndef NDEBUG
    const ResourceAddress &address = *request2.translate.address;
    assert(address.type == ResourceAddress::Type::HTTP ||
           address.type == ResourceAddress::Type::LHTTP ||
           address.type == ResourceAddress::Type::NFS ||
           address.IsCgiAlike());
#endif

    proxy_collect_cookies(request2, headers);

    response_handler.InvokeResponse(&request2, status, std::move(headers),
                                    body);
}

static void
proxy_abort(GError *error, void *ctx)
{
    auto &request2 = *(Request *)ctx;

    response_handler.InvokeAbort(&request2, error);
}

static const struct http_response_handler proxy_response_handler = {
    .response = proxy_response,
    .abort = proxy_abort,
};

void
proxy_handler(Request &request2)
{
    struct pool &pool = request2.pool;
    const TranslateResponse &tr = *request2.translate.response;
    ResourceAddress address = *request2.translate.address;

    assert(address.type == ResourceAddress::Type::HTTP ||
           address.type == ResourceAddress::Type::LHTTP ||
           address.type == ResourceAddress::Type::NFS ||
           address.IsCgiAlike());

    if (request2.translate.response->transparent &&
        (!request2.uri.args.IsEmpty() ||
         !request2.uri.path_info.IsEmpty()))
        address = address.WithArgs(pool,
                                   request2.uri.args,
                                   request2.uri.path_info);

    if (!request2.processor_focus)
        /* forward query string */
        address = address.WithQueryStringFrom(pool, request2.request.uri);

    if (address.IsCgiAlike() &&
        address.GetCgi().script_name == nullptr &&
        address.GetCgi().uri == nullptr)
        /* pass the "real" request URI to the CGI (but without the
           "args", unless the request is "transparent") */
        address.GetCgi().uri = ForwardURI(request2);

    request2.cookie_uri = address.GetUriPath();

    auto forward = request_forward(request2,
                                   tr.request_header_forward,
                                   GetCookieHost(request2), GetCookieURI(request2),
                                   address.IsAnyHttp());

#ifdef SPLICE
    if (forward.body != nullptr)
        forward.body = istream_pipe_new(&pool, *forward.body,
                                        request2.instance.pipe_stock);
#endif

    for (const auto &i : tr.request_headers)
        forward.headers.Add(i.key, i.value);

    request2.instance.cached_resource_loader
        ->SendRequest(pool,
                      request2.session_id.GetClusterHash(),
                      forward.method, address, HTTP_STATUS_OK,
                      std::move(forward.headers), forward.body, nullptr,
                      proxy_response_handler, &request2,
                      request2.async_ref);
}
