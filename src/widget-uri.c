/*
 * Determine the real URI of a widget.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget.h"
#include "uri.h"
#include "args.h"

#include <assert.h>

void
widget_determine_real_uri(pool_t pool, struct widget *widget)
{
    const char *path_info;

    assert(widget != NULL);

    widget->real_uri = widget->class->uri;

    if (widget->from_request.path_info != NULL)
        path_info = widget->from_request.path_info;
    else if (widget->path_info != NULL)
        path_info = widget->path_info;
    else
        path_info = "";

    if (!strref_is_empty(&widget->from_request.query_string))
        widget->real_uri = p_strncat(pool,
                                     widget->real_uri, strlen(widget->real_uri),
                                     path_info, strlen(path_info),
                                     "?", (size_t)1,
                                     widget->from_request.query_string.data,
                                     widget->from_request.query_string.length,
                                     NULL);
    else if (*path_info != 0)
        widget->real_uri = p_strcat(pool,
                                    widget->real_uri,
                                    path_info,
                                    NULL);

    if (widget->query_string != NULL)
        widget->real_uri = p_strcat(pool,
                                    widget->real_uri,
                                    strchr(widget->real_uri, '?') == NULL ? "?" : "&",
                                    widget->query_string,
                                    NULL);
}

const char *
widget_absolute_uri(pool_t pool, const struct widget *widget,
                    const char *relative_uri, size_t relative_uri_length)
{
    return uri_absolute(pool, widget->real_uri,
                        relative_uri, relative_uri_length);
}

static const char *
widget_proxy_uri(pool_t pool,
                 const struct parsed_uri *external_uri,
                 strmap_t args,
                 const struct widget *widget)
{
    const char *path, *args2;

    path = widget_path(pool, widget);

    args2 = args_format(pool, args,
                        "frame", path,
                        "focus", path,
                        NULL);

    return p_strncat(pool,
                     external_uri->base.data,
                     external_uri->base.length,
                     ";", (size_t)1,
                     args2, strlen(args2),
                     NULL);
}

const char *
widget_translation_uri(pool_t pool,
                       const struct parsed_uri *external_uri,
                       strmap_t args,
                       const char *translation)
{
    const char *args2;

    args2 = args_format(pool, args,
                        "translate", translation,
                        NULL, NULL,
                        "frame");

    return p_strncat(pool,
                     external_uri->base.data,
                     external_uri->base.length,
                     ";", (size_t)1,
                     args2, strlen(args2),
                     NULL);
}

const char *
widget_external_uri(pool_t pool,
                    const struct parsed_uri *external_uri,
                    strmap_t args,
                    const struct widget *widget,
                    const char *relative_uri, size_t relative_uri_length)
{
    const char *new_uri;
    const char *args2;

    if (relative_uri_length == 6 &&
        memcmp(relative_uri, ";proxy", 6) == 0)
        /* XXX this special URL syntax should be redesigned */
        return widget_proxy_uri(pool, external_uri, args, widget);

    if (relative_uri_length >= 11 &&
        memcmp(relative_uri, ";translate=", 11) == 0)
        /* XXX this special URL syntax should be redesigned */
        return widget_translation_uri(pool, external_uri, args,
                                      p_strndup(pool, relative_uri + 11,
                                                relative_uri_length - 11));

    new_uri = widget_absolute_uri(pool, widget, relative_uri, relative_uri_length);

    if (widget->id == NULL ||
        external_uri == NULL ||
        widget->class == NULL)
        return new_uri;

    if (new_uri == NULL)
        new_uri = p_strndup(pool, relative_uri, relative_uri_length);

    new_uri = widget_class_relative_uri(widget->class, new_uri);
    if (new_uri == NULL)
        return NULL;

    /* the URI is relative to the widget's base URI.  Convert the URI
       into an absolute URI to the template page on this server and
       add the appropriate args. */
    args2 = args_format(pool, args,
                        "focus", widget->id,
                        "path", new_uri,
                        NULL);

    return p_strncat(pool,
                     external_uri->base.data,
                     external_uri->base.length,
                     ";", (size_t)1,
                     args2, strlen(args2),
                     NULL);
}
