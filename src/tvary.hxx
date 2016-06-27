/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_TRANSLATE_VARY_HXX
#define BENG_PROXY_TRANSLATE_VARY_HXX

struct pool;
class StringMap;
struct TranslateResponse;
class GrowingBuffer;

StringMap *
add_translation_vary_header(struct pool *pool, StringMap *headers,
                            const TranslateResponse *response);

void
write_translation_vary_header(GrowingBuffer *headers,
                              const TranslateResponse *response);

#endif
