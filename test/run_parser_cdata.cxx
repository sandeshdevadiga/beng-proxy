#include "xml_parser.hxx"
#include "istream/istream.hxx"
#include "istream/istream_file.hxx"
#include "fb_pool.hxx"

#include <glib.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static bool should_exit;

class MyXmlParserHandler final : public XmlParserHandler {
public:
    /* virtual methods from class XmlParserHandler */
    bool OnXmlTagStart(gcc_unused const XmlParserTag &tag) override {
        return false;
    }

    void OnXmlTagFinished(gcc_unused const XmlParserTag &tag) override {}
    void OnXmlAttributeFinished(gcc_unused const XmlParserAttribute &attr) override {}

    size_t OnXmlCdata(const char *p, size_t length, gcc_unused bool escaped,
                      gcc_unused off_t start) override {
        (void)write(1, p, length);
        return length;
    }

    void OnXmlEof(gcc_unused off_t length) override {
        should_exit = true;
    }

    void OnXmlError(GError *error) override {
        fprintf(stderr, "ABORT: %s\n", error->message);
        g_error_free(error);
        exit(2);
    }
};

int main(int argc, char **argv) {
    struct pool *root_pool, *pool;
    Istream *istream;

    (void)argc;
    (void)argv;

    fb_pool_init(false);

    root_pool = pool_new_libc(NULL, "root");

    pool = pool_new_linear(root_pool, "test", 8192);

    istream = istream_file_new(pool, "/dev/stdin", (off_t)-1, nullptr);

    MyXmlParserHandler handler;
    auto *parser = parser_new(*pool, *istream, handler);

    while (!should_exit)
        parser_read(parser);

    pool_unref(pool);
    pool_unref(root_pool);
    pool_commit();

    pool_recycler_clear();

    fb_pool_deinit();
}
