/*
 * The BENG request struct.  This is only used by the handlers
 * (handler.c, file-handler.c etc.).
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_REQUEST_HXX
#define BENG_PROXY_REQUEST_HXX

#include "uri_parser.hxx"
#include "translate_request.hxx"
#include "translate_response.hxx"
#include "penv.hxx"
#include "async.hxx"
#include "session.hxx"
#include "transformation.hxx"
#include "widget_class.hxx"
#include "glibfwd.hxx"

class HttpHeaders;

struct request {
    struct client_connection *connection;

    struct http_server_request *request;
    struct parsed_uri uri;

    struct strmap *args;

    struct strmap *cookies;

    /**
     * The name of the session cookie.
     */
    const char *session_cookie;

    SessionId session_id;
    struct session_id_string session_id_string;
    bool send_session_cookie;

    /**
     * The realm name of the request.  This is valid only after the
     * translation server has responded, because the translation
     * server may override it.
     */
    const char *realm;

    /**
     * The realm name of the session.
     */
    const char *session_realm;

    /**
     * Is this request "stateless", i.e. is session management
     * disabled?  This is initialized by request_determine_session(),
     * and may be disabled later by handle_translated_request().
     */
    bool stateless;

    struct {
        TranslateRequest request;
        const TranslateResponse *response;

        const struct resource_address *address;

        /**
         * The next transformation.
         */
        const Transformation *transformation;

        /**
         * The next transformation from the
         * #TRANSLATE_CONTENT_TYPE_LOOKUP response.  These are applied
         * before other transformations.
         */
        const Transformation *suffix_transformation;

        /**
         * A pointer to the "previous" translate response, non-nullptr
         * only if beng-proxy sends a second translate request with a
         * CHECK packet.
         */
        const TranslateResponse *previous;

        /**
         * Number of CHECK packets followed so far.  This variable is
         * used for loop detection.
         */
        unsigned n_checks;

        unsigned n_internal_redirects;

        unsigned n_read_file;

        /**
         * Number of FILE_NOT_FOUND packets followed so far.  This
         * variable is used for loop detection.
         */
        unsigned n_file_not_found;

        /**
         * Number of #TRANSLATE_DIRECTORY_INDEX packets followed so
         * far.  This variable is used for loop detection.
         */
        unsigned n_directory_index;

        unsigned n_probe_path_suffixes;

        /**
         * The Content-Type returned by suffix_registry_lookup().
         */
        const char *content_type;

        char *enotdir_uri;
        const char *enotdir_path_info;
        struct resource_address enotdir_address;

        /**
         * Did we see #TRANSLATE_WANT with #TRANSLATE_USER?  If so,
         * and the user gets modified (see #user_modified), then we
         * need to repeat the initial translation with the new user
         * value.
         */
        bool want_user;

        /**
         * Did we receive #TRANSLATE_USER which modified the session's
         * "user" attribute?  If so, then we need to repeat the
         * initial translation with the new user value.
         */
        bool user_modified;
    } translate;

    /**
     * The URI used for the cookie jar.  This is only used by
     * proxy_handler().
     */
    const char *cookie_uri;

    /**
     * The product token (RFC 2616 3.8) being forwarded; nullptr if
     * beng-proxy shall generate one.
     */
    const char *product_token;

#ifndef NO_DATE_HEADER
    /**
     * The "date" response header (RFC 2616 14.18) being forwarded;
     * nullptr if beng-proxy shall generate one.
     */
    const char *date;
#endif

    /**
     * An identifier for the source stream of the current
     * transformation.  This is used by the filter cache to address
     * resources.
     */
    const char *resource_tag;

    struct processor_env env;

    /**
     * A pointer to the request body, or nullptr if there is none.  Once
     * the request body has been "used", this pointer gets cleared.
     */
    struct istream *body;

    /**
     * Is the processor active, and is there a focused widget?
     */
    bool processor_focus;

    /**
     * Was the response already transformed?  The error document only
     * applies to the original, untransformed response.
     */
    bool transformed;

#ifndef NDEBUG
    bool response_sent;
#endif

    /**
     * This attribute represents the operation that handles the HTTP
     * request.  It is used to clean up resources on abort.
     */
    struct async_operation operation;

    struct async_operation_ref async_ref;

    /**
     * Submit the #TranslateResponse to the translation cache.
     */
    void SubmitTranslateRequest();

    void OnTranslateResponse(const TranslateResponse &response);
    void OnTranslateResponseAfterAuth(const TranslateResponse &response);
    void OnTranslateResponse2(const TranslateResponse &response);

    /**
     * Apply and verify #TRANSLATE_REALM.
     */
    void ApplyTranslateRealm(const TranslateResponse &response);

    /**
     * Copy the packets #TRANSLATE_SESSION, #TRANSLATE_USER,
     * #TRANSLATE_LANGUAGE from the #TranslateResponse to the
     * #session.
     *
     * @return the session (to be released by the caller if not
     * nullptr)
     */
    Session *ApplyTranslateSession(const TranslateResponse &response);

    bool CheckHandleReadFile(const TranslateResponse &response);
    bool CheckHandleProbePathSuffixes(const TranslateResponse &response);
    bool CheckHandleRedirect(const TranslateResponse &response);
    bool CheckHandleBounce(const TranslateResponse &response);
    bool CheckHandleStatus(const TranslateResponse &response);
    bool CheckHandleRedirectBounceStatus(const TranslateResponse &response);

    /**
     * Handle #TRANSLATE_AUTH.
     */
    void HandleAuth(const TranslateResponse &response);

    bool IsTransformationEnabled() const {
        return translate.response->views->transformation != nullptr;
    }

    /**
     * Returns true if the first transformation (if any) is the
     * processor.
     */
    bool IsProcessorFirst() const {
        return IsTransformationEnabled() &&
            translate.response->views->transformation->type
            == Transformation::Type::PROCESS;
    }

    bool IsProcessorEnabled() const;

    bool HasTransformations() const {
        return translate.transformation != nullptr ||
            translate.suffix_transformation != nullptr;
    }

    void CancelTransformations() {
        translate.transformation = nullptr;
        translate.suffix_transformation = nullptr;
    }

    const Transformation *PopTransformation() {
        const Transformation *t = translate.suffix_transformation;
        if (t != nullptr)
            translate.suffix_transformation = t->next;
        else {
            t = translate.transformation;
            if (t != nullptr)
                translate.transformation = t->next;
        }

        return t;
    }
};

/**
 * Discard the request body if it was not used yet.  Call this before
 * sending the response to the HTTP server library.
 */
void
request_discard_body(struct request &request);

void
request_args_parse(struct request &request);

void
request_determine_session(struct request &request);

static inline Session *
request_get_session(const struct request &request)
{
    return request.session_id.IsDefined()
        ? session_get(request.session_id)
        : nullptr;
}

Session *
request_make_session(struct request &request);

void
request_ignore_session(struct request &request);

void
request_discard_session(struct request &request);

void
response_dispatch(struct request &request,
                  http_status_t status, HttpHeaders &&headers,
                  struct istream *body);

void
response_dispatch_message(struct request &request, http_status_t status,
                          const char *msg);

void
response_dispatch_message2(struct request &request, http_status_t status,
                           HttpHeaders &&headers, const char *msg);

void
response_dispatch_error(struct request &request, GError *error);

void
response_dispatch_log(struct request &request, http_status_t status,
                      const char *log_msg);

void
response_dispatch_log(struct request &request, http_status_t status,
                      const char *msg, const char *log_msg);

void
response_dispatch_redirect(struct request &request, http_status_t status,
                           const char *location, const char *msg);

extern const struct http_response_handler response_handler;

#endif
