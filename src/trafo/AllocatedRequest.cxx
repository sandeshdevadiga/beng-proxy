/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "AllocatedRequest.hxx"
#include "translation/Protocol.hxx"

#include "util/Compiler.h"
#include <daemon/log.h>

#include <assert.h>

void
AllocatedTrafoRequest::Parse(TranslationCommand cmd,
                             const void *payload, size_t length)
{
    switch (cmd) {
    case TranslationCommand::BEGIN:
        Clear();

        if (length >= 1)
            protocol_version = *(const uint8_t *)payload;

        break;

    case TranslationCommand::END:
        assert(false);
        gcc_unreachable();

    case TranslationCommand::URI:
        uri_buffer.assign((const char *)payload, length);
        uri = uri_buffer.c_str();
        break;

    case TranslationCommand::HOST:
        host_buffer.assign((const char *)payload, length);
        host = host_buffer.c_str();
        break;

    case TranslationCommand::ARGS:
        args_buffer.assign((const char *)payload, length);
        args = args_buffer.c_str();
        break;

    case TranslationCommand::QUERY_STRING:
        query_string_buffer.assign((const char *)payload, length);
        query_string = query_string_buffer.c_str();
        break;

    case TranslationCommand::USER_AGENT:
        user_agent_buffer.assign((const char *)payload, length);
        user_agent = user_agent_buffer.c_str();
        break;

    case TranslationCommand::UA_CLASS:
        ua_class_buffer.assign((const char *)payload, length);
        ua_class = ua_class_buffer.c_str();
        break;

    case TranslationCommand::LANGUAGE:
        accept_language_buffer.assign((const char *)payload, length);
        accept_language = accept_language_buffer.c_str();
        break;

    case TranslationCommand::AUTHORIZATION:
        authorization_buffer.assign((const char *)payload, length);
        authorization = authorization_buffer.c_str();
        break;

    default:
        daemon_log(4, "unknown translation packet: %u\n", unsigned(cmd));
        break;
    }
}
