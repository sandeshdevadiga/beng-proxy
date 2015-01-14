/*
 * Serve HTTP requests from another HTTP/AJP server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.hxx"
#include "request.hxx"
#include "request_forward.hxx"
#include "http_server.hxx"
#include "http_cache.hxx"
#include "http_response.hxx"
#include "http_address.hxx"
#include "cgi_address.hxx"
#include "global.h"
#include "cookie_client.hxx"
#include "uri-extract.h"
#include "strref_pool.hxx"
#include "strmap.hxx"
#include "http_response.hxx"
#include "istream-impl.h"
#include "lhttp_address.hxx"

gcc_pure
static const char *
GetCookieHost(const request &r)
{
    const TranslateResponse &t = *r.translate.response;
    if (t.cookie_host != nullptr)
        return t.cookie_host;

    const struct resource_address &address = *r.translate.address;
    return resource_address_host_and_port(&address);
}

gcc_pure
static const char *
GetCookieURI(const request &r)
{
    const struct resource_address &address = *r.translate.address;
    return resource_address_uri_path(&address);
}

static void
proxy_collect_cookies(request &request2, const struct strmap *headers)
{
    if (headers == nullptr)
        return;

    auto r = headers->EqualRange("set-cookie2");
    if (r.first == r.second) {
        r = headers->EqualRange("set-cookie");
        if (r.first == r.second)
            return;
    }

    const char *host_and_port = GetCookieHost(request2);
    if (host_and_port == nullptr)
        return;

    const char *path = GetCookieURI(request2);
    if (path == nullptr)
        return;

    struct session *session = request_make_session(request2);
    if (session == nullptr)
        return;

    for (auto i = r.first; i != r.second; ++i)
        cookie_jar_set_cookie2(session->cookies, i->value,
                               host_and_port, path);

    session_put(session);
}

static void
proxy_response(http_status_t status, struct strmap *headers,
               struct istream *body, void *ctx)
{
    request &request2 = *(request *)ctx;

#ifndef NDEBUG
    const struct resource_address &address = *request2.translate.address;
    assert(address.type == RESOURCE_ADDRESS_HTTP ||
           address.type == RESOURCE_ADDRESS_LHTTP ||
           address.type == RESOURCE_ADDRESS_AJP ||
           address.type == RESOURCE_ADDRESS_NFS ||
           resource_address_is_cgi_alike(&address));
#endif

    proxy_collect_cookies(request2, headers);

    response_handler.InvokeResponse(&request2, status, headers, body);
}

static void
proxy_abort(GError *error, void *ctx)
{
    request &request2 = *(request *)ctx;

    response_handler.InvokeAbort(&request2, error);
}

static const struct http_response_handler proxy_response_handler = {
    .response = proxy_response,
    .abort = proxy_abort,
};

void
proxy_handler(request &request2)
{
    struct http_server_request *request = request2.request;
    const TranslateResponse &tr = *request2.translate.response;
    const struct resource_address *address = request2.translate.address;

    assert(address->type == RESOURCE_ADDRESS_HTTP ||
           address->type == RESOURCE_ADDRESS_LHTTP ||
           address->type == RESOURCE_ADDRESS_AJP ||
           address->type == RESOURCE_ADDRESS_NFS ||
           resource_address_is_cgi_alike(address));

    const char *host_and_port = nullptr, *uri_p = nullptr;
    if (address->type == RESOURCE_ADDRESS_HTTP ||
        address->type == RESOURCE_ADDRESS_AJP) {
        host_and_port = address->u.http->host_and_port;
        uri_p = address->u.http->path;
    } else if (address->type == RESOURCE_ADDRESS_LHTTP) {
        host_and_port = address->u.lhttp->host_and_port;
        uri_p = address->u.lhttp->uri;
    }

    struct forward_request forward;
    request_forward(forward, request2,
                    tr.request_header_forward,
                    host_and_port, uri_p,
                    address->type == RESOURCE_ADDRESS_HTTP ||
                    address->type == RESOURCE_ADDRESS_LHTTP);

    if (request2.translate.response->transparent &&
        (!strref_is_empty(&request2.uri.args) ||
         !strref_is_empty(&request2.uri.path_info)))
        address = resource_address_insert_args(*request->pool, address,
                                               request2.uri.args.data,
                                               request2.uri.args.length,
                                               request2.uri.path_info.data,
                                               request2.uri.path_info.length);

    if (!request2.processor_focus)
        /* forward query string */
        address = resource_address_insert_query_string_from(*request->pool,
                                                            address,
                                                            request->uri);

    if (resource_address_is_cgi_alike(address) &&
        address->u.cgi->uri == nullptr) {
        struct resource_address *copy = resource_address_dup(*request->pool,
                                                             address);
        struct cgi_address *cgi = resource_address_get_cgi(copy);

        /* pass the "real" request URI to the CGI (but without the
           "args", unless the request is "transparent") */
        if (request2.translate.response->transparent ||
            strref_is_empty(&request2.uri.args))
            cgi->uri = request->uri;
        else if (strref_is_empty(&request2.uri.query))
            cgi->uri = strref_dup(request->pool, &request2.uri.base);
        else
            cgi->uri = p_strncat(request->pool,
                                 request2.uri.base.data,
                                 request2.uri.base.length,
                                 "?", (size_t)1,
                                 request2.uri.query.data,
                                 request2.uri.query.length,
                                 nullptr);

        address = copy;
    }

#ifdef SPLICE
    if (forward.body != nullptr)
        forward.body = istream_pipe_new(request->pool, forward.body,
                                        global_pipe_stock);
#endif

    for (const auto &i : tr.request_headers)
        forward.headers->Add(i.key, i.value);

    http_cache_request(*global_http_cache, *request->pool,
                       session_id_low(request2.session_id),
                       forward.method, *address,
                       forward.headers, forward.body,
                       proxy_response_handler, &request2,
                       request2.async_ref);
}
