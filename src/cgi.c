/*
 * Run a CGI script.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "cgi.h"
#include "istream-buffer.h"
#include "header-writer.h"
#include "processor.h"
#include "date.h"
#include "format.h"
#include "embed.h"
#include "session.h"
#include "fork.h"
#include "async.h"
#include "header-parser.h"
#include "strutil.h"

#include <daemon/log.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

struct cgi {
    struct istream output;

    istream_t input;
    fifo_buffer_t buffer;
    strmap_t headers;

    struct async_operation async;
    struct http_response_handler_ref handler;
};


static int
cgi_handle_line(struct cgi *cgi, const char *line, size_t length)
{
    assert(cgi != NULL);
    assert(cgi->headers != NULL);
    assert(line != NULL);

    if (length > 0) {
        header_parse_line(cgi->output.pool, cgi->headers,
                          line, length);
        return 0;
    } else
        return 1;
}

static void
cgi_parse_headers(struct cgi *cgi)
{
    const char *buffer, *buffer_end, *start, *end, *next = NULL;
    size_t length;
    int ret = 0;

    buffer = fifo_buffer_read(cgi->buffer, &length);
    if (buffer == NULL)
        return;

    assert(length > 0);
    buffer_end = buffer + length;

    start = buffer;
    while ((end = memchr(start, '\n', buffer_end - start)) != NULL) {
        next = end + 1;
        --end;
        if (likely(*end == '\r'))
            --end;
        while (unlikely(end >= start && char_is_whitespace(*end)))
            --end;

        ret = cgi_handle_line(cgi, start, end - start + 1);
        if (ret)
            break;

        start = next;
    }

    if (next == NULL)
        return;

    fifo_buffer_consume(cgi->buffer, next - buffer);

    if (ret) {
        async_poison(&cgi->async);

        http_response_handler_invoke_response(&cgi->handler,
                                              HTTP_STATUS_OK, cgi->headers,
                                              istream_struct_cast(&cgi->output));
        cgi->headers = NULL;
    }
}

/*
 * input handler
 *
 */

static size_t
cgi_input_data(const void *data, size_t length, void *ctx)
{
    struct cgi *cgi = ctx;

    if (cgi->headers != NULL) {
        void *dest;
        size_t max_length;

        dest = fifo_buffer_write(cgi->buffer, &max_length);
        if (dest == NULL)
            return 0;

        if (length > max_length)
            length = max_length;

        memcpy(dest, data, length);
        fifo_buffer_append(cgi->buffer, length);

        pool_ref(cgi->output.pool);

        cgi_parse_headers(cgi);

        /* we check cgi->input here because this is our indicator that
           cgi->output has been closed; since we are in the cgi->input
           data handler, this is the only reason why cgi->input can be
           NULL */
        if (cgi->input == NULL) {
            pool_unref(cgi->output.pool);
            return 0;
        }

        if (cgi->headers == NULL)
            length += istream_buffer_send(&cgi->output, cgi->buffer);

        pool_unref(cgi->output.pool);

        return length;
    } else {
        if (cgi->buffer != NULL) {
            size_t rest = istream_buffer_consume(&cgi->output, cgi->buffer);
            if (rest > 0)
                return 0;

            cgi->buffer = NULL;
        }

        return istream_invoke_data(&cgi->output, data, length);
    }
}

static ssize_t
cgi_input_direct(istream_direct_t type, int fd, size_t max_length, void *ctx)
{
    struct cgi *cgi = ctx;

    assert(cgi->headers == NULL);

    return istream_invoke_direct(&cgi->output, type, fd, max_length);
}

static void
cgi_input_eof(void *ctx)
{
    struct cgi *cgi = ctx;

    istream_clear_unref(&cgi->input);

    if (cgi->headers != NULL) {
        daemon_log(1, "premature end of headers from CGI script\n");

        istream_invoke_abort(&cgi->output);
    } else if (fifo_buffer_empty(cgi->buffer)) {
        istream_invoke_eof(&cgi->output);
    }
}

static void
cgi_input_abort(void *ctx)
{
    struct cgi *cgi = ctx;

    istream_clear_unref(&cgi->input);
    istream_invoke_abort(&cgi->output);
}

static const struct istream_handler cgi_input_handler = {
    .data = cgi_input_data,
    .direct = cgi_input_direct,
    .eof = cgi_input_eof,
    .abort = cgi_input_abort,
};


/*
 * istream implementation
 *
 */

static inline struct cgi *
istream_to_cgi(istream_t istream)
{
    return (struct cgi *)(((char*)istream) - offsetof(struct cgi, output));
}

static off_t
istream_cgi_available(istream_t istream, int partial)
{
    struct cgi *cgi = istream_to_cgi(istream);
    const void *data;
    size_t available;

    data = fifo_buffer_read(cgi->buffer, &available);
    if (data == NULL)
        available = 0;

    if (cgi->input != NULL) {
        if (cgi->headers != NULL) {
            /* this condition catches the case in cgi_parse_headers():
               http_response_handler_invoke_response() might recursively call
               istream_read(cgi->input) */
            if (partial)
                return available;
            else
                return (off_t)-1;
        }

        available += istream_available(cgi->input, partial);
    }

    return available;
}

static void
istream_cgi_read(istream_t istream)
{
    struct cgi *cgi = istream_to_cgi(istream);

    if (cgi->input != NULL) {
        istream_handler_set_direct(cgi->input, cgi->output.handler_direct);

        /* this condition catches the case in cgi_parse_headers():
           http_response_handler_invoke_response() might recursively call
           istream_read(cgi->input) */
        if (cgi->headers == NULL)
            istream_read(cgi->input);
    } else {
        size_t rest = istream_buffer_consume(&cgi->output, cgi->buffer);
        if (rest == 0)
            istream_invoke_eof(&cgi->output);
    }
}

static void
istream_cgi_close(istream_t istream)
{
    struct cgi *cgi = istream_to_cgi(istream);

    if (cgi->input != NULL)
        istream_close(cgi->input);
    else
        istream_invoke_abort(&cgi->output);
}

static const struct istream istream_cgi = {
    .available = istream_cgi_available,
    .read = istream_cgi_read,
    .close = istream_cgi_close,
};


/*
 * async operation
 *
 */

static struct cgi *
async_to_cgi(struct async_operation *ao)
{
    return (struct cgi*)(((char*)ao) - offsetof(struct cgi, async));
}

static void
cgi_async_abort(struct async_operation *ao)
{
    struct cgi *cgi = async_to_cgi(ao);

    assert(cgi->input != NULL);

    istream_close(cgi->input);
}

static struct async_operation_class cgi_async_operation = {
    .abort = cgi_async_abort,
};


/*
 * constructor
 *
 */

static void attr_noreturn
cgi_run(const char *path,
        http_method_t method, const char *uri, struct strmap *headers)
{
    char *const envp[1] = { NULL };

    (void)method;
    (void)uri;
    (void)headers;

    execle(path, path, NULL, envp);
    fprintf(stderr, "exec('%s') failed: %s\n",
            path, strerror(errno));
    _exit(2);
}

void
cgi_new(pool_t pool,
        const char *path,
        http_method_t method, const char *uri,
        struct strmap *headers, istream_t body,
        const struct http_response_handler *handler,
        void *handler_ctx,
        struct async_operation_ref *async_ref)
{
    struct cgi *cgi = p_malloc(pool, sizeof(*cgi));
    pid_t pid;
    istream_t input;

    http_response_handler_set(&cgi->handler, handler, handler_ctx);

    pid = beng_fork(pool, body, &input);
    if (pid < 0) {
        http_response_handler_invoke_abort(&cgi->handler);
        return;
    }

    if (pid == 0)
        cgi_run(path, method, uri, headers);

    cgi->output = istream_cgi;
    cgi->output.pool = pool;

    istream_assign_ref_handler(&cgi->input, input,
                               &cgi_input_handler, cgi, 0);

    cgi->buffer = fifo_buffer_new(pool, 1024);
    cgi->headers = strmap_new(pool, 32);

    async_init(&cgi->async, &cgi_async_operation);
    async_ref_set(async_ref, &cgi->async);

    istream_read(input);
}
