#!/bin/sh
# Builds newlib for StarlightOS (target x86_64-elf, no libgloss: our syscalls.c replaces it).
#
#   NEWLIB_SRC=/path/to/newlib-4.x ./build-newlib.sh
#
# Get the source from https://sourceware.org/newlib/ (release tarball) or
# `git clone https://sourceware.org/git/newlib-cygwin.git`. Needs x86_64-elf-gcc/binutils on PATH
# (Homebrew: brew install x86_64-elf-gcc; Linux: build binutils+gcc with --target=x86_64-elf).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
NEWLIB_SRC=${NEWLIB_SRC:-$HERE/newlib-src}
SYSROOT=${SYSROOT:-$HERE/../sysroot}
TARGET=x86_64-elf
BUILD=$HERE/build-newlib

[ -f "$NEWLIB_SRC/configure" ] || { echo "error: NEWLIB_SRC ($NEWLIB_SRC) does not contain newlib's configure script" >&2; exit 1; }
command -v $TARGET-gcc >/dev/null || { echo "error: $TARGET-gcc not found in PATH" >&2; exit 1; }

mkdir -p "$BUILD" "$SYSROOT"
cd "$BUILD"

# -mno-red-zone / -mcmodel=small / no PIE match how the kernel and apps are built.
# -mno-sse is NOT used: the kernel enables SSE state, and libgcc/newlib may use it.
CFLAGS_FOR_TARGET="-O2 -g0 -mno-red-zone -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -mcmodel=small" \
"$NEWLIB_SRC/configure" \
    --target=$TARGET \
    --prefix="$SYSROOT" \
    --disable-multilib \
    --disable-libgloss \
    --disable-nls \
    --disable-newlib-supplied-syscalls \
    --disable-newlib-multithread \
    --enable-newlib-io-long-long \
    --enable-newlib-io-c99-formats \
    --enable-newlib-register-fini \
    --disable-newlib-atexit-alloc

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" all-target-newlib
make install-target-newlib
echo "newlib installed in $SYSROOT/$TARGET"
