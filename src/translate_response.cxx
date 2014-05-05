/*
 * The translation response struct.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "translate_response.hxx"
#include "pool.h"
#include "pbuffer.hxx"
#include "strmap.h"
#include "widget_view.hxx"
#include "regex.h"

#include <string.h>

void
TranslateResponse::Clear()
{
    memset(this, 0, sizeof(*this));
}

void
TranslateResponse::CopyFrom(struct pool *pool, const TranslateResponse &src)
{
    protocol_version = src.protocol_version;

    /* we don't copy the "max_age" attribute, because it's only used
       by the tcache itself */

    expires_relative = src.expires_relative;

    status = src.status;

    request_header_forward = src.request_header_forward;
    response_header_forward = src.response_header_forward;

    base = p_strdup_checked(pool, src.base);
    regex = p_strdup_checked(pool, src.regex);
    inverse_regex = p_strdup_checked(pool, src.inverse_regex);
    site = p_strdup_checked(pool, src.site);
    document_root = p_strdup_checked(pool, src.document_root);
    redirect = p_strdup_checked(pool, src.redirect);
    expand_redirect = p_strdup_checked(pool, src.expand_redirect);
    bounce = p_strdup_checked(pool, src.bounce);
    scheme = p_strdup_checked(pool, src.scheme);
    host = p_strdup_checked(pool, src.host);
    uri = p_strdup_checked(pool, src.uri);
    local_uri = p_strdup_checked(pool, src.local_uri);
    untrusted = p_strdup_checked(pool, src.untrusted);
    untrusted_prefix = p_strdup_checked(pool, src.untrusted_prefix);
    untrusted_site_suffix =
        p_strdup_checked(pool, src.untrusted_site_suffix);
    unsafe_base = src.unsafe_base;
    easy_base = src.easy_base;
    regex_tail = src.regex_tail;
    regex_unescape = src.regex_unescape;
    direct_addressing = src.direct_addressing;
    stateful = src.stateful;
    discard_session = src.discard_session;
    secure_cookie = src.secure_cookie;
    filter_4xx = src.filter_4xx;
    previous = src.previous;
    transparent = src.transparent;
    auto_base = src.auto_base;
    widget_info = src.widget_info;
    widget_group = p_strdup_checked(pool, src.widget_group);
    test_path = p_strdup_checked(pool, src.test_path);

    strset_init(&container_groups);
    strset_copy(pool, &container_groups, &src.container_groups);

    anchor_absolute = src.anchor_absolute;
    dump_headers = src.dump_headers;
    session = nullptr;

    check = DupBuffer(pool, src.check);
    want_full_uri = DupBuffer(pool, src.want_full_uri);

    /* The "user" attribute must not be present in cached responses,
       because they belong to only that one session.  For the same
       reason, we won't copy the user_max_age attribute. */
    user = nullptr;

    language = nullptr;
    realm = p_strdup_checked(pool, src.realm);
    www_authenticate = p_strdup_checked(pool, src.www_authenticate);
    authentication_info = p_strdup_checked(pool, src.authentication_info);
    cookie_domain = p_strdup_checked(pool, src.cookie_domain);
    cookie_host = p_strdup_checked(pool, src.cookie_host);

    headers = src.headers != nullptr
        ? strmap_dup(pool, src.headers, 17)
        : nullptr;

    views = src.views != nullptr
        ? widget_view_dup_chain(pool, src.views)
        : nullptr;

    vary = DupBuffer(pool, src.vary);
    invalidate = DupBuffer(pool, src.invalidate);
    want = DupBuffer(pool, src.want);
    file_not_found = DupBuffer(pool, src.file_not_found);
    content_type_lookup = DupBuffer(pool, src.content_type_lookup);
    content_type = p_strdup_checked(pool, src.content_type);
    directory_index = DupBuffer(pool, src.directory_index);
    error_document = DupBuffer(pool, src.error_document);

    validate_mtime.mtime = src.validate_mtime.mtime;
    validate_mtime.path =
        p_strdup_checked(pool, src.validate_mtime.path);
}

bool
TranslateResponse::IsExpandable() const
{
    return regex != nullptr &&
        (expand_redirect != nullptr ||
         resource_address_is_expandable(&address) ||
         widget_view_any_is_expandable(views));
}

bool
TranslateResponse::Expand(struct pool *pool,
                          const GMatchInfo *match_info, GError **error_r)
{
    assert(pool != nullptr);
    assert(regex != nullptr);
    assert(match_info != nullptr);

    if (expand_redirect != nullptr) {
        redirect = expand_string_unescaped(pool, expand_redirect,
                                           match_info, error_r);
        if (redirect == nullptr)
            return false;
    }

    return resource_address_expand(pool, &address, match_info, error_r) &&
        widget_view_expand_all(pool, views, match_info, error_r);
}
