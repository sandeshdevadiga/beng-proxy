include version.mk

CC = gcc
CFLAGS = -O0 -g -DPOISON -DDEBUG_POOL_REF -DSPLICE
LDFLAGS =

MACHINE := $(shell uname -m)
ifeq ($(MACHINE),x86_64)
ARCH_CFLAGS = -march=athlon64
else ifeq ($(MACHINE),i686)
ARCH_CFLAGS = -march=pentium4
else
ARCH_CFLAGS =
endif

ifeq ($(O),1)
CFLAGS = -O3 -g -DNDEBUG -DSPLICE
endif

ifeq ($(PROFILE),1)
CFLAGS = -O3 -g -DNDEBUG -DSPLICE -DPROFILE -pg
LDFLAGS = -lc_p -pg
endif

WARNING_CFLAGS = -Wall -W -pedantic -Werror -pedantic-errors -std=gnu99 -Wmissing-prototypes -Wwrite-strings -Wcast-qual -Wfloat-equal -Wshadow -Wpointer-arith -Wbad-function-cast -Wsign-compare -Waggregate-return -Wmissing-declarations -Wmissing-noreturn -Wmissing-format-attribute -Wredundant-decls -Wnested-externs -Winline -Wdisabled-optimization -Wno-long-long -Wstrict-prototypes -Wundef

ifeq ($(ICC),1)
CC = icc
ARCH_CFLAGS = -march=pentium4
WARNING_CFLAGS = -std=gnu99 -x c -Wall -Werror -wd981
endif

MORE_CFLAGS = -DVERSION=\"$(VERSION)\"

ALL_CFLAGS = $(CFLAGS) $(ARCH_CFLAGS) $(MORE_CFLAGS) $(WARNING_CFLAGS) 

LIBDAEMON_CFLAGS := $(shell pkg-config --cflags libcm4all-daemon)
LIBDAEMON_LIBS := $(shell pkg-config --libs libcm4all-daemon)

LIBEVENT_CFLAGS =
LIBEVENT_LIBS = -L/usr/local/lib -levent

LIBATTR_CFLAGS =
LIBATTR_LIBS = -lattr

SOURCES = src/main.c \
	src/child.c \
	src/session.c \
	src/connection.c \
	src/handler.c \
	src/file-handler.c \
	src/proxy-handler.c \
	src/replace.c \
	src/widget.c \
	src/processor.c \
	src/parser.c \
	src/embed.c \
	src/wembed.c \
	src/socket-util.c \
	src/listener.c \
	src/client-socket.c \
	src/buffered-io.c \
	src/header-parser.c \
	src/header-writer.c \
	src/http-body.c \
	src/http-server.c \
	src/http-client.c \
	src/url-stream.c \
	src/fifo-buffer.c \
	src/growing-buffer.c \
	src/istream-memory.c \
	src/istream-null.c \
	src/istream-string.c \
	src/istream-file.c \
	src/istream-chunked.c \
	src/istream-dechunk.c \
	src/istream-cat.c \
	src/istream-pipe.c \
	src/istream-delayed.c \
	src/istream-hold.c \
	src/uri.c \
	src/args.c \
	src/gmtime.c \
	src/date.c \
	src/strutil.c \
	src/format.c \
	src/strmap.c \
	src/pstring.c \
	src/pool.c

HEADERS = $(wildcard src/*.h)

OBJECTS = $(patsubst %.c,%.o,$(SOURCES))

.PHONY: all clean

all: src/beng-proxy

clean:
	rm -f src/beng-proxy src/*.o doc/beng.{log,aux,ps,pdf,html} vgcore* core* gmon.out test/*.o test/benchmark-gmtime test/format-http-date

src/beng-proxy: $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBEVENT_LIBS) $(LIBDAEMON_LIBS) $(LIBATTR_LIBS)

$(OBJECTS): %.o: %.c $(HEADERS)
	$(CC) -c -o $@ $< $(ALL_CFLAGS) $(LIBEVENT_CFLAGS) $(LIBDAEMON_CFLAGS) $(LIBATTR_CFLAGS)

test/%.o: test/%.c $(HEADERS)
	$(CC) -c -o $@ $< $(ALL_CFLAGS) $(LIBEVENT_CFLAGS) $(LIBDAEMON_CFLAGS) $(LIBATTR_CFLAGS) -Isrc

test/benchmark-gmtime: test/benchmark-gmtime.o src/gmtime.o test/libcore-gmtime.o
	$(CC) -o $@ $^ $(LDFLAGS)

test/format-http-date: test/format-http-date.o src/gmtime.o src/date.o
	$(CC) -o $@ $^ $(LDFLAGS)

profile: CFLAGS = -O3 -DNDEBUG -DSPLICE -DPROFILE -g -pg
profile: LDFLAGS = -lc_p -pg
profile: src/beng-proxy
	./src/beng-proxy

# -DNO_DATE_HEADER -DNO_XATTR -DNO_LAST_MODIFIED_HEADER
benchmark: CFLAGS = -O3 -DNDEBUG -DALWAYS_INLINE
benchmark: src/beng-proxy
	./src/beng-proxy

valgrind: CFLAGS = -O0 -g -DPOISON -DVALGRIND
valgrind: src/beng-proxy
	valgrind --show-reachable=yes --leak-check=yes ./src/beng-proxy

doc/beng.pdf: doc/beng.tex
	cd $(dir $<) && pdflatex $(notdir $<)

doc/beng.dvi: doc/beng.tex
	cd $(dir $<) && latex $(notdir $<)
