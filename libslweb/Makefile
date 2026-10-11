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

# M9.4: HTTPS runs the kernel's TLS client. tls.c/crypto.c/x509.c are shared
# verbatim with the kernel; tls_user.c provides the ring-3 platform layer
# (sl_* sockets, newlib malloc/time, the kernel CSPRNG via SL_SYS_RANDOM).
build/tls.o: ../tls.c ../tls.h ../crypto.h ../x509.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/crypto.o: ../crypto.c ../crypto.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/chachapoly.o: ../chachapoly.c ../crypto.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/p256.o: ../p256.c ../crypto.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/bignum.o: ../bignum.c ../bignum.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/rsa.o: ../rsa.c ../rsa.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/ecdsa.o: ../ecdsa.c ../ecdsa.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/x509.o: ../x509.c ../x509.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/tls_user.o: tls_user.c ../user/slnet.h ../net.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libslweb.a: build/url.o build/http.o build/html.o build/dom.o build/layout.o build/css.o build/image.o build/png.o build/cache.o build/form.o \
                  build/tls.o build/tls_user.o build/crypto.o build/chachapoly.o build/p256.o build/bignum.o build/rsa.o build/ecdsa.o build/x509.o
	$(AR) rcs $@ $^

install: all
	mkdir -p $(SYSROOT)/lib/starlight $(NEWLIB_INC)
	cp build/libslweb.a $(SYSROOT)/lib/starlight/
	cp slweb.h $(NEWLIB_INC)/

clean:
	rm -rf build

.PHONY: all install clean
