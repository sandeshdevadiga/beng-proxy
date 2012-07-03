/*
 * Handler for HTTP requests.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "handler.h"
#include "file-handler.h"
#include "request.h"
#include "connection.h"
#include "config.h"
#include "args.h"
#include "session.h"
#include "instance.h"
#include "tcache.h"
#include "growing-buffer.h"
#include "header-writer.h"
#include "strref-pool.h"
#include "dpool.h"
#include "http-server.h"
#include "transformation.h"
#include "expiry.h"
#include "uri-escape.h"
#include "strutil.h"
#include "strmap.h"
#include "istream.h"
#include "translate-client.h"
#include "ua_classification.h"

#include <daemon/log.h>

#include <assert.h>

static const char *
bounce_uri(struct pool *pool, const struct request *request,
           const struct translate_response *response)
{
    const char *scheme = response->scheme != NULL
        ? response->scheme : "http";
    const char *host = response->host != NULL
        ? response->host
        : strmap_get(request->request->headers, "host");
    if (host == NULL)
        host = "localhost";

    const char *uri_path = response->uri != NULL
        ? p_strncat(pool, response->uri, strlen(response->uri),
                    ";", strref_is_empty(&request->uri.args) ? (size_t)0 : 1,
                    request->uri.args.data, request->uri.args.length,
                    "?", strref_is_empty(&request->uri.query) ? (size_t)0 : 1,
                    request->uri.query.data, request->uri.query.length,
                    NULL)
        : request->request->uri;

    const char *current_uri = p_strcat(pool, scheme, "://", host, uri_path,
                                       NULL);
    const char *escaped_uri = uri_escape_dup(pool, current_uri,
                                             strlen(current_uri), '%');

    return p_strcat(pool, response->bounce, escaped_uri, NULL);
}

/**
 * Determine the realm name, consider the override by the translation
 * server.  Guaranteed to return non-NULL.
 */
static const char *
get_request_realm(struct pool *pool, const struct strmap *request_headers,
                  const struct translate_response *response)
{
    assert(response != NULL);

    if (response->realm != NULL)
        return response->realm;

    const char *host = strmap_get_checked(request_headers, "host");
    if (host != NULL) {
        char *p = p_strdup(pool, host);
        str_to_lower(p);
        return p;
    }

    /* fall back to empty string as the default realm if there is no
       "Host" header */
    return "";
}

static void
handle_translated_request(struct request *request,
                          const struct translate_response *response)
{
    request->realm = get_request_realm(request->request->pool,
                                       request->request->headers, response);

    if (request->session_realm != NULL &&
        strcmp(request->realm, request->session_realm) != 0) {
        daemon_log(2, "ignoring spoofed session id from another realm (request='%s', session='%s')\n",
                   request->realm, request->session_realm);
        request_ignore_session(request);
    }

    struct session *session;

    request->connection->site_name = response->site;

    if (response->transparent) {
        session_id_clear(&request->session_id);
        request->stateless = true;
        request->args = NULL;
    }

    if (response->discard_session)
        request_discard_session(request);
    else if (response->transparent)
        request_ignore_session(request);

    request->translate.response = response;
    request->translate.transformation = response->views != NULL
        ? response->views->transformation
        : NULL;

    if (response->request_header_forward.modes[HEADER_GROUP_COOKIE] != HEADER_FORWARD_MANGLE ||
        response->response_header_forward.modes[HEADER_GROUP_COOKIE] != HEADER_FORWARD_MANGLE) {
        /* disable session management if cookies are not mangled by
           beng-proxy */
        session_id_clear(&request->session_id);
        request->stateless = true;
    }

    if (response->status == (http_status_t)-1 ||
        (response->status == (http_status_t)0 &&
         response->address.type == RESOURCE_ADDRESS_NONE &&
         response->www_authenticate == NULL &&
         response->bounce == NULL &&
         response->redirect == NULL)) {
        response_dispatch_message(request, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                  "Internal server error");
        return;
    }

    if (response->session != NULL || response->user != NULL ||
        response->language != NULL || response->views->transformation != NULL)
        session = request_get_session(request);
    else
        session = NULL;

    if (response->session != NULL) {
        if (*response->session == 0) {
            /* clear translate session */

            if (session != NULL)
                session_clear_translate(session);
        } else {
            /* set new translate session */

            if (session == NULL)
                session = request_make_session(request);

            if (session != NULL)
                session_set_translate(session, response->session);
        }
    }

    if (response->user != NULL) {
        if (*response->user == 0) {
            /* log out */

            if (session != NULL)
                session_clear_user(session);
        } else {
            /* log in */

            if (session == NULL)
                session = request_make_session(request);

            if (session != NULL)
                session_set_user(session, response->user,
                                 response->user_max_age);
        }
    } else if (session != NULL && session->user != NULL && session->user_expires > 0 &&
               is_expired(session->user_expires)) {
        daemon_log(4, "user '%s' has expired\n", session->user);
        d_free(session->pool, session->user);
        session->user = NULL;
    }

    if (response->language != NULL) {
        if (*response->language == 0) {
            /* reset language setting */

            if (session != NULL)
                session_clear_language(session);
        } else {
            /* override language */

            if (session == NULL)
                session = request_make_session(request);

            if (session != NULL)
                session_set_language(session, response->language);
        }
    }

    /* always enforce sessions when the processor is enabled */
    if (request_processor_enabled(request) && session == NULL)
        session = request_make_session(request);

    if (session != NULL)
        session_put(session);

    request->resource_tag = resource_address_id(&response->address,
                                                request->request->pool);

    request->processor_focus = request->args != NULL &&
        request_processor_enabled(request) &&
        strmap_get(request->args, "focus") != NULL;

    if (response->address.type == RESOURCE_ADDRESS_LOCAL) {
        if (response->address.u.local.delegate != NULL)
            delegate_handler(request);
        else
            file_callback(request);
    } else if (response->address.type == RESOURCE_ADDRESS_HTTP ||
               resource_address_is_cgi_alike(&response->address) ||
               response->address.type == RESOURCE_ADDRESS_AJP) {
        proxy_handler(request);
    } else if (response->redirect != NULL) {
        int status = response->status != 0
            ? response->status : HTTP_STATUS_SEE_OTHER;
        response_dispatch_redirect(request, status, response->redirect, NULL);
    } else if (response->bounce != NULL) {
        response_dispatch_redirect(request, HTTP_STATUS_SEE_OTHER,
                                   bounce_uri(request->request->pool, request,
                                              response),
                                   NULL);
    } else if (response->status != (http_status_t)0) {
        response_dispatch(request, response->status, NULL, NULL);
    } else if (response->www_authenticate != NULL) {
        response_dispatch_message(request, HTTP_STATUS_UNAUTHORIZED,
                                  "Unauthorized");
    } else {
        daemon_log(2, "empty response from translation server\n");

        response_dispatch_message(request, HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                  "Internal server error");
    }
}

static const struct translate_handler handler_translate_handler;

static void
handler_translate_response(const struct translate_response *response,
                           void *ctx)
{
    struct request *request = ctx;

    if (!strref_is_null(&response->check)) {
        /* repeat request with CHECK set */

        if (++request->translate.checks > 4) {
            daemon_log(2, "got too many consecutive CHECK packets\n");
            response_dispatch_message(request,
                                      HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                      "Internal server error");
            return;
        }

        request->translate.previous = response;
        request->translate.request.check = response->check;

        translate_cache(request->request->pool,
                        request->connection->instance->translate_cache,
                        &request->translate.request,
                        &handler_translate_handler, request,
                        &request->async_ref);
        return;
    }

    if (response->previous) {
        if (request->translate.previous == NULL) {
            daemon_log(2, "no previous translation response\n");
            response_dispatch_message(request,
                                      HTTP_STATUS_INTERNAL_SERVER_ERROR,
                                      "Internal server error");
            return;
        }

        response = request->translate.previous;
    }

    handle_translated_request(request, response);
}

static void
handler_translate_error(GError *error, void *ctx)
{
    struct request *request = ctx;

    daemon_log(1, "translation error on '%s': %s\n",
               request->request->uri, error->message);

    static const struct translate_response error_response = {
        .status = -1,
    };

    /* a lot of code in response.c dereferences the
       #translate_response pointer, so we need a valid pointer here */
    request->translate.response = &error_response;
    request->translate.transformation = NULL;

    /* pretend this error was generated by the translation client, so
       the HTTP client sees a 500 and not a 404 (if the translation
       server is not running) */
    if (error->domain != translate_quark()) {
        error->domain = translate_quark();
        error->code = 0;
    }

    response_dispatch_error(request, error);
    g_error_free(error);
}

static const struct translate_handler handler_translate_handler = {
    .response = handler_translate_response,
    .error = handler_translate_error,
};

static bool
request_uri_parse(struct request *request2, struct parsed_uri *dest)
{
    const struct http_server_request *request = request2->request;
    bool ret;

    ret = uri_parse(dest, request->uri);
    if (!ret) {
        /* response_dispatch() assumes that we have a translation
           response, and will dereference it - at this point, the
           translation server hasn't been queried yet, so we just
           insert an empty response here */
        static const struct translate_response tr_error = {
            .status = -1,
        };

        request2->translate.response = &tr_error;
        request2->translate.transformation = NULL;

        response_dispatch_message(request2, HTTP_STATUS_BAD_REQUEST,
                                  "Malformed URI");
    }

    return ret;
}

static void
fill_translate_request(struct translate_request *t,
                       const struct http_server_request *request,
                       const struct parsed_uri *uri,
                       struct strmap *args)
{
    t->local_address = request->local_address;
    t->local_address_length = request->local_address_length;
    t->remote_host = request->remote_address;
    t->host = strmap_get(request->headers, "host");
    t->user_agent = strmap_get(request->headers, "user-agent");
    t->ua_class = t->user_agent != NULL
        ? ua_classification_lookup(t->user_agent)
        : NULL;
    t->accept_language = strmap_get(request->headers, "accept-language");
    t->authorization = strmap_get(request->headers, "authorization");
    t->uri = strref_dup(request->pool, &uri->base);
    t->args = args != NULL
        ? args_format(request->pool, args,
                      NULL, NULL, NULL, NULL,
                      "translate")
        : NULL;
    if (t->args != NULL && *t->args == 0)
        t->args = NULL;

    t->query_string = strref_is_empty(&uri->query)
        ? NULL
        : strref_dup(request->pool, &uri->query);
    t->widget_type = NULL;
    strref_null(&t->check);
    t->error_document_status = 0;
}

static void
ask_translation_server(struct request *request2, struct tcache *tcache)
{
    request2->translate.previous = NULL;
    request2->translate.checks = 0;

    struct http_server_request *request = request2->request;
    fill_translate_request(&request2->translate.request, request2->request,
                           &request2->uri, request2->args);
    translate_cache(request->pool, tcache, &request2->translate.request,
                    &handler_translate_handler, request2,
                    &request2->async_ref);
}

static void
serve_document_root_file(struct request *request2,
                         const struct config *config)
{
    struct http_server_request *request = request2->request;
    struct parsed_uri *uri;
    struct translate_response *tr;
    const char *index_file = NULL;
    bool process;

    uri = &request2->uri;

    request2->translate.response = tr = p_calloc(request->pool,
                                                 sizeof(*request2->translate.response));

    if (uri->base.data[uri->base.length - 1] == '/') {
        index_file = "index.html";
        process = true;
    } else {
        process = strref_ends_with_n(&uri->base, ".html", 5);
    }

    if (process) {
        struct transformation *transformation = p_malloc(request->pool, sizeof(*transformation));
        struct widget_view *view = p_malloc(request->pool, sizeof(*view));
        widget_view_init(view);

        transformation->next = NULL;
        transformation->type = TRANSFORMATION_PROCESS;

        view->transformation = transformation;

        tr->views = view;
    } else {
        struct widget_view *view = p_malloc(request->pool, sizeof(*view));
        widget_view_init(view);

        tr->views = view;
        tr->transparent = true;
    }

    request2->translate.transformation = tr->views->transformation;

    tr->address.type = RESOURCE_ADDRESS_LOCAL;
    tr->address.u.local.path = p_strncat(request->pool,
                                         config->document_root,
                                         strlen(config->document_root),
                                         uri->base.data,
                                         uri->base.length,
                                         index_file, (size_t)10,
                                         NULL);

    tr->request_header_forward = (struct header_forward_settings){
        .modes = {
            [HEADER_GROUP_IDENTITY] = HEADER_FORWARD_MANGLE,
            [HEADER_GROUP_CAPABILITIES] = HEADER_FORWARD_YES,
            [HEADER_GROUP_COOKIE] = HEADER_FORWARD_MANGLE,
            [HEADER_GROUP_OTHER] = HEADER_FORWARD_NO,
            [HEADER_GROUP_FORWARD] = HEADER_FORWARD_NO,
        },
    };

    tr->response_header_forward = (struct header_forward_settings){
        .modes = {
            [HEADER_GROUP_IDENTITY] = HEADER_FORWARD_NO,
            [HEADER_GROUP_CAPABILITIES] = HEADER_FORWARD_YES,
            [HEADER_GROUP_COOKIE] = HEADER_FORWARD_MANGLE,
            [HEADER_GROUP_OTHER] = HEADER_FORWARD_NO,
            [HEADER_GROUP_FORWARD] = HEADER_FORWARD_NO,
        },
    };

    request2->resource_tag = tr->address.u.local.path;
    request2->processor_focus = process &&
        strmap_get_checked(request2->args, "focus") != NULL;

    file_callback(request2);
}

/*
 * async operation
 *
 */

static struct request *
async_to_request(struct async_operation *ao)
{
    return (struct request *)(((char *)ao) - offsetof(struct request, operation));
}

static void
handler_abort(struct async_operation *ao)
{
    struct request *request2 = async_to_request(ao);

    request_discard_body(request2);

    /* forward the abort to the http_server library */
    async_abort(&request2->async_ref);
}

static const struct async_operation_class handler_operation = {
    .abort = handler_abort,
};

/*
 * constructor
 *
 */

void
handle_http_request(struct client_connection *connection,
                    struct http_server_request *request,
                    struct async_operation_ref *async_ref)
{
    struct request *request2;
    bool ret;

    assert(request != NULL);

    request2 = p_malloc(request->pool, sizeof(*request2));
    request2->connection = connection;
    request2->request = request;
    request2->product_token = NULL;

    request2->args = NULL;
    request2->cookies = NULL;
    session_id_clear(&request2->session_id);
    request2->send_session_cookie = NULL;
#ifdef DUMP_WIDGET_TREE
    request2->dump_widget_tree = NULL;
#endif
    request2->body = http_server_request_has_body(request)
        ? istream_hold_new(request->pool, request->body)
        : NULL;
    request2->transformed = false;

    async_init(&request2->operation, &handler_operation);
    async_ref_set(async_ref, &request2->operation);

#ifndef NDEBUG
    request2->response_sent = false;
#endif

    ret = request_uri_parse(request2, &request2->uri);
    if (!ret)
        return;

    assert(!strref_is_empty(&request2->uri.base));
    assert(request2->uri.base.data[0] == '/');

    request_args_parse(request2);
    request_determine_session(request2);

    if (connection->instance->translate_cache == NULL)
        serve_document_root_file(request2, connection->config);
    else
        ask_translation_server(request2, connection->instance->translate_cache);
}
