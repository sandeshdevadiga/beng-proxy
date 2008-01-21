/*
 * Emulation layer for Google gadgets.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "google-gadget-internal.h"
#include "widget.h"
#include "istream.h"
#include "url-stream.h"
#include "http-response.h"
#include "parser.h"
#include "processor.h"

static void
google_send_error(struct google_gadget *gw, const char *msg)
{
    istream_t response = istream_string_new(gw->pool, msg);
    istream_delayed_set(gw->delayed, response);
    gw->delayed = NULL;

    if (gw->parser != NULL)
        parser_close(gw->parser);
    else if (async_ref_defined(&gw->async))
        async_abort(&gw->async);

    pool_unref(gw->pool);

    istream_read(response);
}

static istream_t
google_gadget_process(const struct google_gadget *gw, istream_t istream)
{
    return processor_new(gw->pool, istream,
                         gw->widget, gw->env,
                         PROCESSOR_JSCRIPT);
}

static void
gg_set_content(struct google_gadget *gg, istream_t istream)
{
    assert(gg != NULL);
    assert(gg->delayed != NULL);

    if (gg->has_locale && gg->waiting_for_locale) {
        assert(gg->raw == NULL);

        gg->raw = istream;
    } else {
        istream_delayed_set(gg->delayed, google_gadget_process(gg, istream));
        gg->delayed = NULL;
    }
}


/*
 * url_stream handler (HTML contents)
 *
 */

static void
google_gadget_content_response(http_status_t status, strmap_t headers,
                               istream_t body, void *ctx)
{
    struct google_gadget *gw = ctx;
    const char *p;

    assert(gw->delayed != NULL);

    async_ref_clear(&gw->async);

    if (!http_status_is_success(status)) {
        if (body != NULL)
            istream_close(body);

        google_send_error(gw, "content server reported error");
        return;
    }

    p = strmap_get(headers, "content-type");
    if (p == NULL || strncmp(p, "text/html", 9) != 0 || body == NULL) {
        if (body != NULL)
            istream_close(body);

        google_send_error(gw, "text/html expected");
        return;
    }

    gg_set_content(gw, body);
}

static void
google_gadget_content_abort(void *ctx)
{
    struct google_gadget *gw = ctx;

    assert(gw->delayed != NULL);

    async_ref_clear(&gw->async);

    istream_free(&gw->delayed);
    pool_unref(gw->pool);
}

static const struct http_response_handler google_gadget_content_handler = {
    .response = google_gadget_content_response,
    .abort = google_gadget_content_abort,
};


/*
 * istream implementation which serves the CDATA section in <Content/>
 *
 */

static inline struct google_gadget *
istream_to_google_gadget(istream_t istream)
{
    return (struct google_gadget *)(((char*)istream) - offsetof(struct google_gadget, output));
}

static void
istream_google_html_read(istream_t istream)
{
    struct google_gadget *gw = istream_to_google_gadget(istream);

    assert(gw->parser != NULL);
    assert(gw->from_parser.sending_content);

    parser_read(gw->parser);
}

static void
istream_google_html_close(istream_t istream)
{
    struct google_gadget *gw = istream_to_google_gadget(istream);

    assert(gw->parser != NULL);
    assert(gw->from_parser.sending_content);

    parser_close(gw->parser);
}

static const struct istream istream_google_html = {
    .read = istream_google_html_read,
    .close = istream_google_html_close,
};


/*
 * msg callbacks
 *
 */

void
google_gadget_msg_eof(struct google_gadget *gg)
{
    /* XXX */
    (void)gg;

    assert(gg->has_locale && gg->waiting_for_locale);

    gg->waiting_for_locale = 0;

    if (gg->raw != NULL) {
        gg_set_content(gg, gg->raw);
        istream_read(gg->raw);
    }
}

void
google_gadget_msg_abort(struct google_gadget *gg)
{
    /* XXX */
    google_gadget_msg_eof(gg);
}


/*
 * produce output
 *
 */

static void
google_content_tag_finished(struct google_gadget *gw,
                            const struct parser_tag *tag)
{
    switch (gw->from_parser.type) {
    case TYPE_NONE:
        break;

    case TYPE_HTML:
        gw->from_parser.sending_content = 1;

        if (tag->type == TAG_OPEN) {
            gw->output = istream_google_html;
            gw->output.pool = gw->pool;

            gg_set_content(gw, istream_struct_cast(&gw->output));
        } else {
            /* it's TAG_SHORT, handle that gracefully */
            istream_delayed_set(gw->delayed, istream_null_new(gw->pool));
            gw->delayed = NULL;
        }

        return;

    case TYPE_URL:
        url_stream_new(gw->pool, gw->env->http_client_stock,
                       HTTP_METHOD_GET, gw->from_parser.url,
                       NULL, NULL,
                       &google_gadget_content_handler, gw,
                       &gw->async);

        return;
    }

    google_send_error(gw, "malformed google gadget");
}


/*
 * parser callbacks
 *
 */

static void
google_parser_tag_start(const struct parser_tag *tag, void *ctx)
{
    struct google_gadget *gw = ctx;

    if (gw->from_parser.sending_content) {
        gw->from_parser.sending_content = 0;
        istream_invoke_eof(&gw->output);
    }

    if (!gw->has_locale && tag->type != TAG_CLOSE &&
        strref_cmp_literal(&tag->name, "locale") == 0) {
        gw->from_parser.tag = TAG_LOCALE;
        gw->has_locale = 1;
        gw->waiting_for_locale = 0;
    } else if (strref_cmp_literal(&tag->name, "content") == 0) {
        gw->from_parser.tag = TAG_CONTENT;
    } else {
        gw->from_parser.tag = TAG_NONE;
    }
}

static void
google_parser_tag_finished(const struct parser_tag *tag, void *ctx)
{
    struct google_gadget *gw = ctx;

    if (tag->type != TAG_CLOSE &&
        gw->from_parser.tag == TAG_CONTENT &&
        gw->delayed != NULL) {
        gw->from_parser.tag = TAG_NONE;
        google_content_tag_finished(gw, tag);
    } else {
        gw->from_parser.tag = TAG_NONE;
    }
}

static void
google_parser_attr_finished(const struct parser_attr *attr, void *ctx)
{
    struct google_gadget *gw = ctx;

    switch (gw->from_parser.tag) {
    case TAG_NONE:
        break;

    case TAG_LOCALE:
        if (strref_cmp_literal(&attr->name, "messages") == 0 &&
            !strref_is_empty(&attr->value) &&
            gw->delayed != NULL) {
            google_gadget_msg_load(gw, strref_dup(gw->pool, &attr->value));
            gw->waiting_for_locale = 1;
            gw->raw = NULL;
        }
        break;

    case TAG_CONTENT:
        if (strref_cmp_literal(&attr->name, "type") == 0) {
            if (strref_cmp_literal(&attr->value, "url") == 0)
                gw->from_parser.type = TYPE_URL;
            else if (strref_cmp_literal(&attr->value, "html") == 0)
                gw->from_parser.type = TYPE_HTML;
            else {
                google_send_error(gw, "unknown type attribute");
                return;
            }
        } else if (gw->from_parser.type == TYPE_URL &&
                   strref_cmp_literal(&attr->name, "href") == 0) {
            gw->from_parser.url = strref_dup(gw->pool, &attr->value);
        }

        break;
    }
}

static size_t
google_parser_cdata(const char *p, size_t length, int escaped, void *ctx)
{
    struct google_gadget *gw = ctx;

    if (!escaped && gw->from_parser.sending_content) {
        if (gw->has_locale && gw->waiting_for_locale)
            return 0;
        return istream_invoke_data(&gw->output, p, length);
    } else
        return length;
}

static void
google_parser_eof(void *ctx, off_t attr_unused length)
{
    struct google_gadget *gw = ctx;

    gw->parser = NULL;

    if (gw->from_parser.sending_content) {
        gw->from_parser.sending_content = 0;
        istream_invoke_eof(&gw->output);
    } else if (gw->delayed != NULL && !async_ref_defined(&gw->async))
        google_send_error(gw, "google gadget did not contain a valid Content element");

    pool_unref(gw->pool);
}

static void
google_parser_abort(void *ctx)
{
    struct google_gadget *gw = ctx;

    gw->parser = NULL;

    if (gw->from_parser.sending_content) {
        gw->from_parser.sending_content = 0;
        istream_invoke_abort(&gw->output);
    } else if (gw->delayed != NULL)
        google_send_error(gw, "google gadget retrieval aborted");

    pool_unref(gw->pool);
}

static const struct parser_handler google_parser_handler = {
    .tag_start = google_parser_tag_start,
    .tag_finished = google_parser_tag_finished,
    .attr_finished = google_parser_attr_finished,
    .cdata = google_parser_cdata,
    .eof = google_parser_eof,
    .abort = google_parser_abort,
};


/*
 * url_stream handler (gadget description)
 *
 */

static void
google_gadget_http_response(http_status_t status, strmap_t headers,
                            istream_t body, void *ctx)
{
    struct google_gadget *gw = ctx;
    const char *p;

    assert(gw->delayed != NULL);

    async_ref_clear(&gw->async);

    if (!http_status_is_success(status)) {
        if (body != NULL)
            istream_close(body);

        google_send_error(gw, "widget server reported error");
        return;
    }

    p = strmap_get(headers, "content-type");
    if (p == NULL || body == NULL ||
        (strncmp(p, "text/xml", 8) != 0 &&
         strncmp(p, "application/xml", 15) != 0)) {
        if (body != NULL)
            istream_close(body);

        google_send_error(gw, "text/xml expected");
        return;
    }

    gw->from_parser.tag = TAG_NONE;
    gw->from_parser.type = TYPE_NONE;
    gw->from_parser.sending_content = 0;
    gw->parser = parser_new(gw->pool, body,
                            &google_parser_handler, gw);
    istream_read(body);
}

static void
google_gadget_http_abort(void *ctx)
{
    struct google_gadget *gw = ctx;

    assert(gw->delayed != NULL);

    async_ref_clear(&gw->async);

    istream_free(&gw->delayed);

    pool_unref(gw->pool);
}

static const struct http_response_handler google_gadget_handler = {
    .response = google_gadget_http_response,
    .abort = google_gadget_http_abort,
};

static void
google_delayed_abort(void *ctx)
{
    struct google_gadget *gw = ctx;

    gw->delayed = NULL;

    if (gw->parser != NULL)
        parser_close(gw->parser);
    else if (async_ref_defined(&gw->async))
        async_abort(&gw->async);
}


/*
 * constructor
 *
 */

istream_t
embed_google_gadget(pool_t pool, struct processor_env *env,
                    struct widget *widget)
{
    struct google_gadget *gw;

    assert(widget != NULL);
    assert(widget->class != NULL);

    pool_ref(pool);

    gw = p_malloc(pool, sizeof(*gw));
    gw->pool = pool;
    gw->env = env;
    gw->widget = widget;
    gw->delayed = istream_delayed_new(pool, google_delayed_abort, gw);
    gw->subst = istream_subst_new(pool, gw->delayed);
    gw->parser = NULL;
    gw->has_locale = 0;

    url_stream_new(pool, env->http_client_stock,
                   HTTP_METHOD_GET, widget->class->uri,
                   NULL, NULL,
                   &google_gadget_handler, gw,
                   &gw->async);

    return gw->subst;
}
