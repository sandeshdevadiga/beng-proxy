/*
 * Escape and unescape in URI style ('%20').
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_PURI_ESCAPE_HXX
#define BENG_PROXY_PURI_ESCAPE_HXX

#include <inline/compiler.h>

#include <stddef.h>

struct pool;

gcc_pure gcc_malloc
const char *
uri_escape_dup(struct pool *pool, const char *src, size_t src_length,
               char escape_char='%');

char *
uri_unescape_dup(struct pool *pool, const char *src, size_t length,
                 char escape_char='%');

#endif
