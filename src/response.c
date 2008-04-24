/*
 * Utilities for transforming the HTTP response being sent.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "request.h"
#include "http-server.h"
#include "header-writer.h"
#include "widget.h"
#include "embed.h"
#include "proxy-widget.h"
#include "session.h"
#include "filter.h"
#include "access-log.h"
#include "uri-address.h"

static const char *const copy_headers[] = {
    "age",
    "etag",
    "cache-control",
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    "last-modified",
    "retry-after",
    "vary",
    NULL,
};

static const char *const copy_headers_processed[] = {
    "cache-control",
    "content-language",
    "content-type",
    "vary",
    NULL,
};


static const char *
request_absolute_uri(struct http_server_request *request)
{
    const char *host = strmap_get(request->headers, "host");

    if (host == NULL)
        return NULL;

    return p_strcat(request->pool,
                    "http://",
                    host,
                    request->uri,
                    NULL);
}


/*
 * processor invocation
 *
 */

static void
response_invoke_processor(struct request *request2,
                          http_status_t status, growing_buffer_t response_headers,
                          istream_t body,
                          const struct translate_transformation *transformation)
{
    struct http_server_request *request = request2->request;
    istream_t request_body;
    struct widget *widget;

    assert(!request2->response_sent);
    assert(body == NULL || !istream_has_handler(body));

    if (body == NULL) {
        response_dispatch(request2, status, response_headers, NULL);
        return;
    }

    if (http_server_request_has_body(request) && !request2->body_consumed) {
        request_body = request->body;
        request2->body_consumed = 1;
    } else {
        request_body = NULL;
    }

    pool_ref(request->pool);

    request_make_session(request2);

    processor_env_init(request->pool, &request2->env,
                       request2->translate_cache,
                       request2->http_cache,
                       request->remote_host,
                       request_absolute_uri(request),
                       &request2->uri,
                       request2->args,
                       request2->session,
                       request->headers,
                       request_body);

    widget = p_malloc(request->pool, sizeof(*widget));
    widget_init(widget, &root_widget_class);
    widget->lazy.path = "";
    widget->lazy.prefix = "__";
    widget->from_request.session = session_get_widget(request2->env.session,
                                                      strref_dup(request->pool,
                                                                 &request2->uri.base),
                                                      true);

    widget->from_request.focus_ref = widget_ref_parse(request->pool,
                                                      strmap_remove(request2->env.args, "focus"));

    widget->from_request.proxy_ref = widget_ref_parse(request->pool,
                                                      strmap_get(request2->env.args, "frame"));
    if (widget->from_request.proxy_ref != NULL) {
        processor_new(request->pool, body, widget, &request2->env,
                      transformation->u.processor_options,
                      &widget_proxy_handler, request,
                      request2->async_ref);

        pool_unref(request->pool);
    } else {
        processor_new(request->pool, body, widget, &request2->env,
                      transformation->u.processor_options,
                      &response_handler, request2,
                      request2->async_ref);
    }

    /*
#ifndef NO_DEFLATE
    if (http_client_accepts_encoding(request->headers, "deflate")) {
        header_write(response_headers, "content-encoding", "deflate");
        body = istream_deflate_new(request->pool, body);
    }
#endif
    */
}


/*
 * dispatch
 *
 */

void
response_dispatch(struct request *request2,
                  http_status_t status, struct growing_buffer *headers,
                  istream_t body)
{
    const struct translate_transformation *transformation
        = request2->translate.transformation;

    assert(!request2->response_sent);
    assert(body == NULL || !istream_has_handler(body));

    if (transformation)
        request2->translate.transformation = transformation->next;

    if (transformation != NULL &&
        transformation->type == TRANSFORMATION_FILTER) {
        struct http_server_request *request = request2->request;

        assert(transformation->u.filter != NULL);

        pool_ref(request->pool);

        filter_new(request->pool,
                   request2->http_client_stock,
                   uri_address_new(request->pool, transformation->u.filter),
                   headers,
                   body,
                   &response_handler, request2,
                   request2->async_ref);
    } else if (transformation != NULL &&
               transformation->type == TRANSFORMATION_PROCESS) {
        response_invoke_processor(request2, status, headers, body,
                                  transformation);
    } else {
        access_log(request2->request, status, body);

        header_write(headers, "server", "beng-proxy v" VERSION);

        request2->response_sent = 1;
        http_server_response(request2->request,
                             status, headers, body);
    }
}


/*
 * HTTP response handler
 *
 */

static void
response_response(http_status_t status, strmap_t headers,
                  istream_t body,
                  void *ctx)
{
    struct request *request2 = ctx;
    struct http_server_request *request = request2->request;
    pool_t pool = request->pool;
    growing_buffer_t response_headers;

    assert(!request2->response_sent);
    assert(body == NULL || !istream_has_handler(body));

    if (headers == NULL) {
        response_headers = growing_buffer_new(request->pool, 1024);
    } else {
        response_headers = growing_buffer_new(request->pool, 2048);
        if (request2->translate.transformation != NULL &&
            request2->translate.transformation->type == TRANSFORMATION_PROCESS)
            headers_copy(headers, response_headers, copy_headers_processed);
        else
            headers_copy(headers, response_headers, copy_headers);
    }

    response_dispatch(request2,
                      status, response_headers,
                      body);

    pool_unref(pool);
}

static void
response_abort(void *ctx)
{
    struct request *request = ctx;
    pool_t pool = request->request->pool;

    if (!request->response_sent)
        http_server_send_message(request->request,
                                 HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                 "Internal server error");

    pool_unref(pool);
}

const struct http_response_handler response_handler = {
    .response = response_response,
    .abort = response_abort,
};
