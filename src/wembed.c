/*
 * Embed a widget.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "embed.h"
#include "uri.h"
#include "processor.h"
#include "widget.h"

#include <assert.h>
#include <string.h>

istream_t
embed_widget_callback(pool_t pool, const struct processor_env *env,
                      struct widget *widget)
{
    http_method_t method = HTTP_METHOD_GET;
    off_t request_content_length = 0;
    istream_t request_body = NULL;

    assert(pool != NULL);
    assert(env != NULL);
    assert(env->widget_callback == embed_widget_callback);
    assert(widget != NULL);

    switch (widget->display) {
        const char *iframe;

    case WIDGET_DISPLAY_INLINE:
        break;

    case WIDGET_DISPLAY_IFRAME:
        /* generate IFRAME element; the client will perform a second
           request for the frame contents, see
           frame_widget_callback() */

        if (widget->id == NULL)
            return istream_string_new(pool, "[framed widget without id]"); /* XXX */

        iframe = p_strcat(pool, "<iframe src='",
                          env->external_uri->base,
                          ";frame=", widget->id,
                          "&", widget->id, "=",
                          widget->append_uri == NULL ? "" : widget->append_uri,
                          "'></iframe>",
                          NULL);
        return istream_string_new(pool, iframe);
    }

    if (widget->id != NULL && env->focus != NULL &&
        (env->external_uri->query != NULL || env->request_body != NULL) &&
        strcmp(widget->id, env->focus) == 0) {
        /* we're in focus.  forward query string and request body. */
        widget->real_uri = p_strncat(pool,
                                     widget->real_uri, strlen(widget->real_uri),
                                     "?", 1,
                                     env->external_uri->query,
                                     env->external_uri->query_length,
                                     NULL);

        if (env->request_body != NULL) {
            method = HTTP_METHOD_POST; /* XXX which method? */
            request_content_length = env->request_content_length;
            request_body = istream_hold_new(pool, env->request_body);
            /* XXX what if there is no stream handler? or two? */
        }
    }

    return embed_new(pool,
                     method, widget->real_uri,
                     request_content_length, request_body,
                     widget,
                     env, PROCESSOR_BODY);
}
