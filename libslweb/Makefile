# libslweb -- userland browser library (URL + HTTP + HTML->text) for StarlightOS.
#
#   make -C libslweb            build build/libslweb.a
#   make -C libslweb install    copy the archive + slweb.h into $(SYSROOT)
#
# A program then just `#include <slweb.h>` and links with -lslweb (starlight-gcc
# adds it automatically once installed).
CC      = x86_64-elf-gcc
AR      = x86_64-elf-ar
SYSROOT ?= $(CURDIR)/../sysroot

# Headers come from the newlib install; only needed to compile the sources.
NEWLIB_INC = $(SYSROOT)/x86_64-elf/include

CFLAGS = -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone \
         -fno-asynchronous-unwind-tables -fno-unwind-tables -m64 -isystem $(NEWLIB_INC)

all: build/libslweb.a

build:
	mkdir -p build

build/url.o: url.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/html.o: html.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/http.o: http.c slweb.h ../user/slnet.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/dom.o: dom.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/layout.o: layout.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/css.o: css.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/image.o: image.c slweb.h png.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/png.o: png.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/cache.o: cache.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/form.o: form.c slweb.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libslweb.a: build/url.o build/http.o build/html.o build/dom.o build/layout.o build/css.o build/image.o build/png.o build/cache.o build/form.o
	$(AR) rcs $@ $^

install: all
	mkdir -p $(SYSROOT)/lib/starlight $(NEWLIB_INC)
	cp build/libslweb.a $(SYSROOT)/lib/starlight/
	cp slweb.h $(NEWLIB_INC)/

clean:
	rm -rf build

.PHONY: all install clean
