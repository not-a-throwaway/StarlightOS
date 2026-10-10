# StarlightOS libc glue for newlib.
#
#   make -C libc newlib      download-free build of newlib itself (needs NEWLIB_SRC, see build-newlib.sh)
#   make -C libc             crt0.o + libstarlight.a  (the OS-specific half)
#   make -C libc install     copy everything into $(SYSROOT)
#
# Result: $(SYSROOT)/bin/starlight-gcc  -- compile C programs for StarlightOS with newlib.

CC      = x86_64-elf-gcc
AR      = x86_64-elf-ar
SYSROOT ?= $(CURDIR)/../sysroot
NEWLIB_SRC ?= $(CURDIR)/newlib-src

# Headers/libs for the *glue* come from the newlib install; only needed to compile syscalls.c.
NEWLIB_INC = $(SYSROOT)/x86_64-elf/include

CFLAGS = -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone \
         -fno-asynchronous-unwind-tables -fno-unwind-tables -m64 -isystem $(NEWLIB_INC)

all: build/crt0.o build/libstarlight.a

build:
	mkdir -p build

build/crt0.o: crt0.S | build
	$(CC) -m64 -c $< -o $@

build/syscalls.o: syscalls.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libstarlight.a: build/syscalls.o
	$(AR) rcs $@ $^

newlib:
	NEWLIB_SRC=$(NEWLIB_SRC) SYSROOT=$(SYSROOT) ./build-newlib.sh

install: all
	mkdir -p $(SYSROOT)/lib/starlight $(SYSROOT)/bin
	cp build/crt0.o build/libstarlight.a starlight.ld $(SYSROOT)/lib/starlight/
	install -m755 starlight-gcc $(SYSROOT)/bin/starlight-gcc

clean:
	rm -rf build

.PHONY: all newlib install clean
