/*
 * FastCGI client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_FCGI_CLIENT_HXX
#define BENG_PROXY_FCGI_CLIENT_HXX

#include "FdType.hxx"

#include <http/method.h>

struct pool;
class EventLoop;
class Istream;
class Lease;
class StringMap;
class HttpResponseHandler;
struct async_operation_ref;
template<typename T> struct ConstBuffer;

/**
 * Sends a HTTP request on a socket to an FastCGI server, and passes
 * the response to the handler.
 *
 * @param pool the memory pool; this client holds a reference until
 * the response callback has returned and the response body is closed
 * @param fd a socket to the HTTP server
 * @param fd_type the exact socket type
 * @param lease the lease for the socket
 * @param method the HTTP request method
 * @param uri the request URI path
 * @param script_filename the absolue path name of the script
 * @param script_name the URI part of the script
 * @param path_info the URI part following the script name
 * @param query_string the query string (without the question mark)
 * @param document_root the absolute path of the document root
 * @param headers the serialized request headers (optional)
 * @param body the request body (optional)
 * @param stderr_fd a file descriptor for #FCGI_STDERR packets (will
 * be closed by this library) or -1 to send everything to stderr
 * @param handler receives the response
 * @param async_ref a handle which may be used to abort the operation
 */
void
fcgi_client_request(struct pool *pool, EventLoop &event_loop,
                    int fd, FdType fd_type, Lease &lease,
                    http_method_t method, const char *uri,
                    const char *script_filename,
                    const char *script_name, const char *path_info,
                    const char *query_string,
                    const char *document_root,
                    const char *remote_addr,
                    const StringMap &headers, Istream *body,
                    ConstBuffer<const char *> params,
                    int stderr_fd,
                    HttpResponseHandler &handler,
                    struct async_operation_ref &async_ref);

#endif
