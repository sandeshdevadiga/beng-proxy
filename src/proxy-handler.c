/*
 * Serve HTTP requests from the local VFS.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "connection.h"
#include "handler.h"
#include "client-socket.h"
#include "http-client.h"
#include "processor.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>

struct proxy_transfer {
    struct http_server_request *request;
    const char *uri;

    client_socket_t client_socket;
    http_client_connection_t http;
    struct http_client_response *response;
    int response_finished;

    processor_t processor;
};

static void
proxy_transfer_close(struct proxy_transfer *pt)
{
    if (pt->processor != NULL)
        processor_free(&pt->processor);

    if (pt->http != NULL) {
        http_client_connection_close(pt->http);
        pt->http = NULL;
        assert(pt->response == NULL);
    }

    if (pt->request != NULL) {
        http_server_connection_free(&pt->request->connection);
        assert(pt->request == NULL);
    }
}

static void
proxy_processor_input(void *ctx)
{
    struct proxy_transfer *pt = ctx;

    http_client_response_read(pt->http);
}

static void
proxy_processor_meta(const char *content_type, off_t length, void *ctx)
{
    struct proxy_transfer *pt = ctx;
    char headers[256];

    http_server_send_status(pt->request->connection, 200);

    snprintf(headers, sizeof(headers), "Content-Type: %s\r\nContent-Length: %lu\r\n\r\n",
             content_type, (unsigned long)length);
    http_server_send(pt->request->connection, headers, strlen(headers));

    http_server_try_write(pt->request->connection);
}

static size_t
proxy_processor_output(const void *data, size_t length, void *ctx)
{
    struct proxy_transfer *pt = ctx;

    return http_server_send(pt->request->connection, data, length);
}

static void
proxy_processor_output_finished(void *ctx)
{
    struct proxy_transfer *pt = ctx;

    http_server_response_finish(pt->request->connection);
}

static void
proxy_processor_free(void *ctx)
{
    struct proxy_transfer *pt = ctx;

    /* XXX when the processor fails, it will close itself and invoke
       this callback */
    if (pt->processor != NULL)
        proxy_transfer_close(pt);
}

static struct processor_handler proxy_processor_handler = {
    .input = proxy_processor_input,
    .meta = proxy_processor_meta,
    .output = proxy_processor_output,
    .output_finished = proxy_processor_output_finished,
    .free = proxy_processor_free,
};

static size_t
proxy_client_response_body(struct http_client_response *response,
                           const void *buffer, size_t length)
{
    struct proxy_transfer *pt = response->handler_ctx;

    /* XXX */
    if (pt->processor == NULL)
        return http_server_send(pt->request->connection, buffer, length);
    else
        return processor_input(pt->processor, buffer, length);
}

static void
proxy_client_response_finished(struct http_client_response *response)
{
    struct proxy_transfer *pt = response->handler_ctx;

    pt->response = NULL;
    pt->response_finished = 1;

    if (pt->processor == NULL) {
        if (pt->request != NULL)
            http_server_response_finish(pt->request->connection);
    } else {
        processor_input_finished(pt->processor);
    }
}

static void
proxy_client_response_free(struct http_client_response *response)
{
    struct proxy_transfer *pt = response->handler_ctx;

    if (!pt->response_finished) {
        /* abort the transfer */
        assert(response == pt->response);
        pt->response = NULL;
        proxy_transfer_close(pt);
    }
}

static struct http_client_request_handler proxy_client_request_handler = {
    .response_body = proxy_client_response_body,
    .response_finished = proxy_client_response_finished,
    .free = proxy_client_response_free,
};

static void 
proxy_http_client_callback(struct http_client_response *response,
                           void *ctx)
{
    struct proxy_transfer *pt = ctx;
    const char *value;

    assert(pt->response == NULL);

    if (response == NULL) {
        pt->http = NULL;
        if (!pt->response_finished)
            proxy_transfer_close(pt);
        return;
    }

    assert(response->content_length >= 0);

    value = strmap_get(response->headers, "content-type");
    if (strncmp(value, "text/html", 9) == 0) {
        pt->processor = processor_new(pt->request->pool,
                                      &proxy_processor_handler, pt);
        if (pt->processor == NULL) {
            /* XXX */
            abort();
        }
    }

    response->handler = &proxy_client_request_handler;
    response->handler_ctx = pt;

    if (pt->processor == NULL) {
        char headers[256];

        http_server_send_status(pt->request->connection, 200);

        snprintf(headers, sizeof(headers), "Content-Length: %lu\r\n\r\n",
                 (unsigned long)response->content_length);
        http_server_send(pt->request->connection, headers, strlen(headers));

        http_server_try_write(pt->request->connection);
    }
}

static const char *const copy_headers[] = {
    "user-agent",
    NULL
};

static void
proxy_client_forward_request(struct proxy_transfer *pt)
{
    strmap_t request_headers;
    const char *value;
    unsigned i;

    assert(pt != NULL);
    assert(pt->http != NULL);
    assert(pt->uri != NULL);

    request_headers = strmap_new(pt->request->pool, 64);

    for (i = 0; copy_headers[i] != NULL; ++i) {
        value = strmap_get(pt->request->headers, copy_headers[i]);
        if (value != NULL)
            strmap_addn(request_headers, copy_headers[i], value);
    }

    http_client_request(pt->http, HTTP_METHOD_GET, pt->uri, request_headers);
}

static void
proxy_client_socket_callback(int fd, int err, void *ctx)
{
    struct proxy_transfer *pt = ctx;

    if (err == 0) {
        assert(fd >= 0);

        pt->http = http_client_connection_new(pt->request->pool, fd,
                                              proxy_http_client_callback, pt);

        proxy_client_forward_request(pt);
    } else {
        fprintf(stderr, "failed to connect: %s\n", strerror(err));
        http_server_send_message(pt->request->connection,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "proxy connect failed");
        http_server_response_finish(pt->request->connection);
    }
}

static size_t proxy_response_body(struct http_server_request *request,
                                  void *buffer, size_t max_length)
{
    struct proxy_transfer *pt = request->handler_ctx;

    (void)pt;
    (void)buffer;
    (void)max_length;

    if (pt->processor == NULL)
        http_client_response_read(pt->http);
    else
        processor_output(pt->processor);

    return 0;
}

static void proxy_response_free(struct http_server_request *request)
{
    struct proxy_transfer *pt = request->handler_ctx;

    assert(request == pt->request);

    request->handler_ctx = NULL;
    pt->request = NULL;

    proxy_transfer_close(pt);
}

static const struct http_server_request_handler proxy_request_handler = {
    .response_body = proxy_response_body,
    .free = proxy_response_free,
};

static int
getaddrinfo_helper(const char *host_and_port, int default_port,
                   const struct addrinfo *hints,
                   struct addrinfo **aip) {
    const char *colon, *host, *port;
    char buffer[256];

    colon = strchr(host_and_port, ':');
    if (colon == NULL) {
        snprintf(buffer, sizeof(buffer), "%d", default_port);

        host = host_and_port;
        port = buffer;
    } else {
        size_t len = colon - host_and_port;

        if (len >= sizeof(buffer)) {
            errno = ENAMETOOLONG;
            return EAI_SYSTEM;
        }

        memcpy(buffer, host_and_port, len);
        buffer[len] = 0;

        host = buffer;
        port = colon + 1;
    }

    if (strcmp(host, "*") == 0)
        host = "0.0.0.0";

    return getaddrinfo(host, port, hints, aip);
}

void
proxy_callback(struct client_connection *connection,
               struct http_server_request *request,
               struct translated *translated)
{
    int ret;
    struct proxy_transfer *pt;
    const char *p, *slash, *host_and_port;
    struct addrinfo hints, *ai;

    (void)connection;

    if (request->method != HTTP_METHOD_HEAD &&
        request->method != HTTP_METHOD_GET) {
        http_server_send_message(request->connection,
                                 HTTP_STATUS_METHOD_NOT_ALLOWED,
                                 "This method is not supported.");
        http_server_response_finish(request->connection);
        return;
    }

    if (memcmp(translated->path, "http://", 7) != 0) {
        /* XXX */
        http_server_send_message(request->connection,
                                 HTTP_STATUS_BAD_REQUEST,
                                 "Invalid proxy URI");
        http_server_response_finish(request->connection);
        return;
    }

    p = translated->path + 7;
    slash = strchr(p, '/');
    if (slash == NULL || slash == p) {
        /* XXX */
        http_server_send_message(request->connection,
                                 HTTP_STATUS_BAD_REQUEST,
                                 "Invalid proxy URI");
        http_server_response_finish(request->connection);
        return;
    }

    host_and_port = p_strndup(request->pool, p, slash - p);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo_helper(host_and_port, 80, &hints, &ai);
    if (ret != 0) {
        fprintf(stderr, "failed to resolve proxy host name\n");
        http_server_send_message(request->connection,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "Internal server error");
        http_server_response_finish(request->connection);
        return;
    }

    pt = p_calloc(request->pool, sizeof(*pt));
    pt->request = request;
    pt->uri = slash;

    ret = client_socket_new(request->pool,
                            ai->ai_addr, ai->ai_addrlen,
                            proxy_client_socket_callback, pt,
                            &pt->client_socket);
    if (ret != 0) {
        perror("client_socket_new() failed");
        http_server_send_message(request->connection,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "Internal server error");
        http_server_response_finish(request->connection);
        return;
    }

    request->handler = &proxy_request_handler;
    request->handler_ctx = pt;
}
