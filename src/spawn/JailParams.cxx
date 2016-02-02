/*
 * JailCGI integration.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "JailParams.hxx"
#include "Prepared.hxx"
#include "pool.hxx"
#include "pexpand.hxx"
#include "util/CharUtil.hxx"
#include "util/ConstBuffer.hxx"

#include <glib.h>

#include <string.h>

gcc_const
static GQuark
jail_quark(void)
{
    return g_quark_from_static_string("jail");
}

void
JailParams::Init()
{
    enabled = false;
    account_id = nullptr;
    site_id = nullptr;
    user_name = nullptr;
    host_name = nullptr;
    home_directory = nullptr;
    expand_home_directory = nullptr;
    host_name = nullptr;
}

bool
JailParams::Check(GError **error_r) const
{
    if (!enabled)
        return true;

    if (home_directory == nullptr && expand_home_directory == nullptr) {
        g_set_error(error_r, jail_quark(), 0, "No JailCGI home directory");
        return false;
    }

    return true;
}

JailParams::JailParams(struct pool *pool, const JailParams &src)
    :enabled(src.enabled),
     account_id(p_strdup_checked(pool, src.account_id)),
     site_id(p_strdup_checked(pool, src.site_id)),
     user_name(p_strdup_checked(pool, src.user_name)),
     host_name(p_strdup_checked(pool, src.host_name)),
     home_directory(p_strdup_checked(pool, src.home_directory)),
     expand_home_directory(p_strdup_checked(pool, src.expand_home_directory))
{
}

void
JailParams::CopyFrom(struct pool &pool, const JailParams &src)
{
    enabled = src.enabled;
    account_id = p_strdup_checked(&pool, src.account_id);
    site_id = p_strdup_checked(&pool, src.site_id);
    user_name = p_strdup_checked(&pool, src.user_name);
    host_name = p_strdup_checked(&pool, src.host_name);
    home_directory = p_strdup_checked(&pool, src.home_directory);
    expand_home_directory = p_strdup_checked(&pool, src.expand_home_directory);
}

char *
JailParams::MakeId(char *p) const
{
    if (enabled) {
        p = (char *)mempcpy(p, ";j=", 3);
        p = stpcpy(p, home_directory);
    }

    return p;
}

bool
JailParams::InsertWrapper(PreparedChildProcess &p,
                          const char *document_root) const
{
    if (!enabled)
        return true;

    StaticArray<const char *, 16> w;

    w.push_back("/usr/lib/cm4all/jailcgi/bin/wrapper");

    if (document_root != nullptr) {
        w.push_back("-d");
        w.push_back(document_root);
    }

    if (account_id != nullptr) {
        w.push_back("--account");
        w.push_back(account_id);
    }

    if (site_id != nullptr) {
        w.push_back("--site");
        w.push_back(site_id);
    }

    if (user_name != nullptr) {
        w.push_back("--name");
        w.push_back(user_name);
    }

    if (host_name != nullptr)
        p.SetEnv("JAILCGI_SERVERNAME", host_name);

    if (home_directory != nullptr) {
        w.push_back("--home");
        w.push_back(home_directory);
    }

    w.push_back("--");

    return p.InsertWrapper({w.raw(), w.size()});
}

bool
JailParams::Expand(struct pool &pool, const MatchInfo &match_info,
                   Error &error_r)
{
    if (expand_home_directory != nullptr) {
        home_directory =
            expand_string_unescaped(&pool, expand_home_directory, match_info,
                                    error_r);
        if (home_directory == nullptr)
            return false;
    }

    return true;
}
