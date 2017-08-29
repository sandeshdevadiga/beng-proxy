/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef BENG_PROXY_LOG_CLIENT_HXX
#define BENG_PROXY_LOG_CLIENT_HXX

#include "net/UniqueSocketDescriptor.hxx"
#include "io/Logger.hxx"
#include "util/ByteOrder.hxx"

#include <beng-proxy/log.h>

#include <string.h>

struct StringView;
struct AccessLogDatagram;

/**
 * A client for the logging protocol.
 */
class LogClient {
    const LLogger logger;

    UniqueSocketDescriptor fd;

    size_t position;
    char buffer[32768];

public:
    explicit LogClient(UniqueSocketDescriptor &&_fd)
        :logger("access_log"), fd(std::move(_fd)) {}

    void Begin() {
        position = 0;
        Append(&log_magic, sizeof(log_magic));
    }

    void Append(const void *p, size_t size) {
        if (position + size <= sizeof(buffer))
            memcpy(buffer + position, p, size);

        position += size;
    }

    void AppendAttribute(enum beng_log_attribute attribute,
                         const void *value, size_t size) {
        const uint8_t attribute8 = (uint8_t)attribute;
        Append(&attribute8, sizeof(attribute8));
        Append(value, size);
    }

    void AppendU8(enum beng_log_attribute attribute, uint8_t value) {
        AppendAttribute(attribute, &value, sizeof(value));
    }

    void AppendU16(enum beng_log_attribute attribute, uint16_t value) {
        const uint16_t value2 = ToBE16(value);
        AppendAttribute(attribute, &value2, sizeof(value2));
    }

    void AppendU64(enum beng_log_attribute attribute, uint64_t value) {
        const uint64_t value2 = ToBE64(value);
        AppendAttribute(attribute, &value2, sizeof(value2));
    }

    void AppendString(enum beng_log_attribute attribute, const char *value);
    void AppendString(enum beng_log_attribute attribute, StringView value);

    bool Commit();

    bool Send(const AccessLogDatagram &d);
};

#endif
