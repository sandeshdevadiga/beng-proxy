/*
 * High level HTTP client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_HTTP_REQUEST_HXX
#define BENG_PROXY_HTTP_REQUEST_HXX

#include <http/method.h>

struct pool;
class EventLoop;
class Istream;
struct TcpBalancer;
struct SocketFilter;
class SocketFilterFactory;
struct HttpAddress;
class HttpResponseHandler;
struct async_operation_ref;
class HttpHeaders;

/**
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 */
void
http_request(struct pool &pool, EventLoop &event_loop,
             TcpBalancer &tcp_balancer,
             unsigned session_sticky,
             const SocketFilter *filter, SocketFilterFactory *filter_factory,
             http_method_t method,
             const HttpAddress &address,
             HttpHeaders &&headers, Istream *body,
             HttpResponseHandler &handler,
             struct async_operation_ref &async_ref);

#endif
