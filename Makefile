# StarlightOS Makefile
#
#   make            build everything (x86_64 kernel, 32-bit kernel, apps)
#   make run        build disk.img and boot it in QEMU (x86_64)
#   make test       boot headless and run the end-to-end tests (needs qemu + python3)
#   make shelltest  host-only shell/VFS tests: control flow, redirects, $(cmd), globs, unix tools (gcc + python3)
#   make hosttest   host-only unit tests: crypto, telnet, HTML renderer (needs gcc, python3, 'cryptography')
#   make slwebtest  host-only unit tests for libslweb: URL, HTTP, HTML, DOM, layout, CSS + images (needs gcc, python3)
#   make tcptest    host-only unit tests for the kernel TCP: window scaling, receive windows (needs gcc)
#   make tlstest    TLS client interop + fuzz against OpenSSL servers (needs gcc, openssl, python3)
#   make shots      render the desktop to PPM files on the host (no QEMU)
#   make clean      remove all build outputs
#
# Toolchains: x86_64-elf-gcc / i686-elf-gcc (e.g. Homebrew: x86_64-elf-gcc i686-elf-gcc).
# QEMU audio backend:  make run QEMU_AUDIO=pa|alsa|coreaudio|dsound|sdl

TARGET = i686-elf
CC = $(TARGET)-gcc
AS = $(TARGET)-as
LD = $(TARGET)-gcc

CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-pie -fno-stack-protector -fno-builtin -m32 -MMD -MP
LDFLAGS = -m32 -ffreestanding -O2 -nostdlib -no-pie -Wl,--build-id=none

# Sources shared by the 32-bit and 64-bit kernels.
SRCS = kernel.c idt.c pic.c pit.c rtc.c speaker.c keyboard.c mouse.c \
       font8x8.c font.c font_ttf.c pci.c rtl8139.c vbe.c vga.c gui.c starlightos_gui.c \
       disk.c vfs.c ext2.c devfs.c procfs.c \
       kstring.c kprintf.c serial.c ktime.c ksched.c kheap.c \
       net.c tcp.c poll.c httpd.c dns.c url.c http.c html.c crypto.c chachapoly.c p256.c krand.c tls.c bignum.c rsa.c ecdsa.c x509.c telnet.c \
       ac97.c audio.c \
       shell.c cmd_text.c cmd_net.c cmd_sound.c cmd_unix.c cmd_posix.c

OBJS = boot.o gdt_asm.o idt_asm.o gdt.o $(SRCS:.c=.o)

all: starlightos64.elf starlightos.bin apps/MINES.ELF

build32: starlightos.bin
build64: starlightos64.elf

boot.o: boot.s
	$(AS) boot.s -o boot.o
gdt_asm.o: gdt.s
	$(AS) gdt.s -o gdt_asm.o
idt_asm.o: idt.s
	$(AS) idt.s -o idt_asm.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

starlightos.bin: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o starlightos.bin $(OBJS) -lgcc
	@echo "Built starlightos.bin"

QEMU_AUDIO ?= coreaudio
QEMU_NETDEV ?= -netdev user,id=net0
QEMU_DEVICES = $(QEMU_NETDEV) -device rtl8139,netdev=net0 -audiodev $(QEMU_AUDIO),id=snd0 -device AC97,audiodev=snd0
QEMU_EXTRA_ARGS ?=

run32: starlightos.bin disk.img
	qemu-system-i386 -drive format=raw,file=disk.img -kernel starlightos.bin $(QEMU_DEVICES) $(QEMU_EXTRA_ARGS)

# ---- x86_64 BIOS build ----
CC64 = x86_64-elf-gcc
AS64 = x86_64-elf-as
LD64 = x86_64-elf-ld
OBJCOPY64 = x86_64-elf-objcopy

AS16 = i686-elf-as
LD16 = i686-elf-ld
OBJCOPY16 = i686-elf-objcopy

CFLAGS64 = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-pie -fno-stack-protector -fno-builtin -mno-red-zone -m64 -MMD -MP

SRCS64 = $(SRCS) process.c
OBJS64 = boot64.o idt64_asm.o process64.o $(SRCS64:.c=_64.o)

boot64.o: x86_64_port/boot64.s
	$(AS64) --64 x86_64_port/boot64.s -o boot64.o
process64.o: process64.s
	$(AS64) --64 process64.s -o process64.o
idt64_asm.o: idt64.s
	$(AS64) --64 idt64.s -o idt64_asm.o

%_64.o: %.c
	$(CC64) $(CFLAGS64) -c $< -o $@

starlightos64.elf: $(OBJS64) x86_64_port/linker64.ld
	$(LD64) -T x86_64_port/linker64.ld -o starlightos64.elf $(OBJS64)
	@echo "Built starlightos64.elf"

starlightos64.bin: starlightos64.elf
	$(OBJCOPY64) -O binary starlightos64.elf starlightos64.bin
	@echo "Built starlightos64.bin"

bootsector.o: x86_64_port/boot16.s
	$(AS16) --32 x86_64_port/boot16.s -o bootsector.o
bootsector.elf: bootsector.o
	$(LD16) -m elf_i386 -Ttext 0x7c00 -o bootsector.elf bootsector.o
bootsector.bin: bootsector.elf
	$(OBJCOPY16) -O binary bootsector.elf bootsector.bin

stage2.o: x86_64_port/stage2.s
	$(AS16) --32 x86_64_port/stage2.s -o stage2.o
stage2.elf: stage2.o
	$(LD16) -m elf_i386 -Ttext 0x8000 -o stage2.elf stage2.o
stage2.bin: stage2.elf
	$(OBJCOPY16) -O binary stage2.elf stage2.bin

apps/MINES.ELF: user/minesweeper.c user/syscall.h apps/minesweeper.c apps/minesweeper.h user/user64.ld
	$(CC64) -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-pie -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -m64 -nostdlib -no-pie -Wl,--build-id=none -Wl,-T,user/user64.ld -Iuser -o $@ user/minesweeper.c apps/minesweeper.c

disk.img: make_disk.py assets/JetBrainsMonoNerdFont-Regular-ASCII.ttf starlightos64.bin bootsector.bin stage2.bin starlightos64.elf apps/MINES.ELF apps/GDEMO.ELF apps/NDEMO.ELF apps/WEBTEST.ELF apps/BROWSER.ELF $(wildcard apps/*.ELF) $(wildcard disk_files/*) user_files/guest/httpd_test/INDEX.HTM
	python3 make_disk.py

run64: starlightos64.elf starlightos64.bin bootsector.bin stage2.bin disk.img
	qemu-system-x86_64 -drive format=raw,file=disk.img,if=ide,index=0 -boot order=c -m 256M $(QEMU_DEVICES) $(QEMU_EXTRA_ARGS)

run: run64
boot: run64
run_hd64: run64

test: disk.img
	python3 tests/run_tests.py

# Boot with host port 8080 forwarded to the guest, for the httpd_test chat room:
#   (in StarlightOS)  cd /home/guest/httpd_test   then   httpd 8080
#   (on the host)     open http://localhost:8080/
run-chat: starlightos64.elf starlightos64.bin bootsector.bin stage2.bin disk.img
	$(MAKE) run64 QEMU_NETDEV='-netdev user,id=net0,hostfwd=tcp::8080-:8080'

# Host-only unit tests for the crypto / TLS / telnet code (no QEMU or cross-compiler needed;
# needs gcc, python3 and the 'cryptography' package).
hosttest:
	gcc -O1 -Wall -Wextra -Itests/host -I. -o /tmp/starlightos_crypto_cli tests/host/crypto_cli.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c
	python3 tests/host/crypto_diff.py /tmp/starlightos_crypto_cli
	gcc -O1 -Wall -Wextra -Itests/host -I. -o /tmp/starlightos_x509_cli tests/host/x509_cli.c x509.c crypto.c bignum.c rsa.c ecdsa.c
	python3 tests/host/x509_diff.py /tmp/starlightos_x509_cli
	gcc -O1 -Wall -Wextra -I. -o /tmp/starlightos_telnet_cli tests/host/telnet_cli.c telnet.c
	python3 tests/host/telnet_test.py /tmp/starlightos_telnet_cli
	gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I. -o /tmp/starlightos_html_cli tests/host/html_cli.c html.c
	python3 tests/host/html_test.py /tmp/starlightos_html_cli

# Runs the real shell + VFS + ext2/devfs/procfs on the host (hardware stubbed) against a copy of disk.img:
# pipes, redirects (2> 2>&1 &>), $(cmd), globbing, if/while/for, scripts with arguments, and the unix tools.
# Needs only gcc and python3, and a built disk.img (make disk.img).
shelltest: disk.img
	gcc -std=gnu99 -O1 -g -w -fno-builtin -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Itests/host -o /tmp/starlightos_shell_host tests/host/shell_host.c shell.c cmd_text.c cmd_unix.c cmd_posix.c vfs.c ext2.c devfs.c procfs.c kstring.c kprintf.c kheap.c ktime.c
	python3 tests/host/shell_test.py /tmp/starlightos_shell_host disk.img

# TLS client interop + fuzz tests against OpenSSL servers on localhost (needs openssl, python3, 'cryptography' not required).
# Covers TLS 1.3 and 1.2, all 3 AEADs, both key-exchange groups, RSA and ECDSA certificates, coalesced records,
# 300 KB transfers, clean refusals, and 150 corrupted-server-flight sessions under ASan/UBSan.
tlstest:
	gcc -g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=all -o /tmp/starlightos_tls_host tests/host/tls_host.c tls.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c x509.c
	gcc -g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=all -DTLS_TEST_NO_EMS -o /tmp/starlightos_tls_host_noems tests/host/tls_host.c tls.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c x509.c
	python3 tests/host/tls_matrix.py /tmp/starlightos_tls_host /tmp/starlightos_tls_host_noems
	python3 tests/host/tls_fuzz.py /tmp/starlightos_tls_host 150

# Renders the real desktop code (gui.c + vga.c + starlightos_gui.c, hardware stubbed) to /tmp/starlightos_shots/*.ppm so
# you can look at TrueColor output without booting QEMU. Open the PPMs in any image viewer.
shots:
	mkdir -p /tmp/starlightos_shots
	gcc -O1 -w -I. -Itests/host -o /tmp/starlightos_render tests/host/render_demo.c vga.c starlightos_gui.c font_ttf.c
	/tmp/starlightos_render assets/JetBrainsMonoNerdFont-Regular-ASCII.ttf /tmp/starlightos_shots/frame
	@echo "wrote /tmp/starlightos_shots/frame_{desktop,terminal,files,about}.ppm"

clean64:
	rm -f *_64.o *_64.d boot64.o idt64_asm.o process64.o
	rm -f apps/minesweeper_64.o apps/minesweeper.o
	rm -f starlightos64.elf starlightos64.bin apps/MINES.ELF
	rm -f bootsector.o bootsector.elf bootsector.bin
	rm -f stage2.o stage2.elf stage2.bin
	rm -f disk.img boot.img starlightos.iso
	rm -rf .iso64_root

clean: clean64
	rm -f $(OBJS) gdt.d $(SRCS:.c=.d) starlightos.bin
	rm -rf libslweb/build

-include gdt.d $(SRCS:.c=.d) $(SRCS64:.c=_64.d)

# poll()/select() core + the event-driven httpd chat room, run on the host against real sockets
# (tests/host/poll_host.c mocks only the TCP layer below them). Needs gcc and python3.
polltest:
	gcc -g -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -DPOLL_HOST_TEST -I. -include string.h -o /tmp/poll_host tests/host/poll_host.c poll.c httpd.c
	/tmp/poll_host unit
	python3 tests/host/poll_test.py /tmp/poll_host

# ---- newlib / hosted C programs --------------------------------------------------------------
# One-time:  NEWLIB_SRC=/path/to/newlib make newlib      (builds newlib + our syscall layer into ./sysroot)
# Then:      make apps/CDEMO.ELF                           (a stdio/malloc/math demo)
# Your own:  sysroot/bin/starlight-gcc -O2 prog.c -o PROG.ELF   and drop PROG.ELF into disk_files/
NEWLIB_SRC ?= $(CURDIR)/libc/newlib-src
SYSROOT ?= $(CURDIR)/sysroot
STARLIGHT_GCC = $(SYSROOT)/bin/starlight-gcc

newlib:
	$(MAKE) -C libc newlib NEWLIB_SRC=$(NEWLIB_SRC) SYSROOT=$(SYSROOT)
	$(MAKE) -C libc install SYSROOT=$(SYSROOT)

apps/CDEMO.ELF: user/cdemo.c libc/syscalls.c libc/crt0.S libc/starlight.ld
	@test -x $(STARLIGHT_GCC) || { echo "newlib is not built yet: run 'NEWLIB_SRC=... make newlib' (see libc/README.md)"; exit 1; }
	$(STARLIGHT_GCC) -O2 -Wall user/cdemo.c -o $@

# GDEMO: the ring-3 window syscalls (fbmap/present/getevents). Rebuilds libstarlight first so the
# freshly added sl_* wrappers are in the archive the linker picks up.
apps/GDEMO.ELF: user/gdemo.c user/slwindow.h libc/syscalls.c libc/crt0.S libc/starlight.ld
	@test -x $(STARLIGHT_GCC) || { echo "newlib is not built yet: run 'NEWLIB_SRC=... make newlib' (see libc/README.md)"; exit 1; }
	$(MAKE) -C libc all install SYSROOT=$(SYSROOT)
	$(STARLIGHT_GCC) -O2 -Wall user/gdemo.c -o $@

# NDEMO: the ring-3 network syscalls (socket/connect/send/recv/poll/resolve, user/slnet.h).
apps/NDEMO.ELF: user/ndemo.c user/slnet.h libc/syscalls.c libc/crt0.S libc/starlight.ld
	@test -x $(STARLIGHT_GCC) || { echo "newlib is not built yet: run 'NEWLIB_SRC=... make newlib' (see libc/README.md)"; exit 1; }
	$(MAKE) -C libc all install SYSROOT=$(SYSROOT)
	$(STARLIGHT_GCC) -O2 -Wall user/ndemo.c -o $@

# libslweb: the userland browser library (URL + HTTP + HTML->text, see libslweb/).
libslweb:
	$(MAKE) -C libslweb all install SYSROOT=$(SYSROOT)

# WEBTEST: the M3 demo -- link libslweb and fetch/render a page over the M2 sockets.
apps/WEBTEST.ELF: user/webtest.c libslweb/slweb.h libslweb/url.c libslweb/http.c libslweb/html.c libc/syscalls.c libc/crt0.S libc/starlight.ld
	@test -x $(STARLIGHT_GCC) || { echo "newlib is not built yet: run 'NEWLIB_SRC=... make newlib' (see libc/README.md)"; exit 1; }
	$(MAKE) -C libc all install SYSROOT=$(SYSROOT)
	$(MAKE) -C libslweb all install SYSROOT=$(SYSROOT)
	$(STARLIGHT_GCC) -O2 -Wall user/webtest.c -o $@

# BROWSER: the M6 browser -- libslweb (DOM + box layout + CSS + images) drawn into an M1 window.
apps/BROWSER.ELF: user/browser.c user/slwindow.h font8x8_basic.h libslweb/slweb.h libslweb/url.c libslweb/http.c libslweb/html.c libslweb/dom.c libslweb/layout.c libslweb/css.c libslweb/image.c libslweb/png.c libslweb/cache.c libslweb/form.c libslweb/tls_user.c tls.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c x509.c libc/syscalls.c libc/crt0.S libc/starlight.ld
	@test -x $(STARLIGHT_GCC) || { echo "newlib is not built yet: run 'NEWLIB_SRC=... make newlib' (see libc/README.md)"; exit 1; }
	$(MAKE) -C libc all install SYSROOT=$(SYSROOT)
	$(MAKE) -C libslweb all install SYSROOT=$(SYSROOT)
	$(STARLIGHT_GCC) -O2 -Wall -I. user/browser.c -o $@

.PHONY: newlib run-chat polltest shelltest hosttest tlstest tcptest shots all build32 build64 run run32 run64 run_hd64 boot clean clean64 test libslweb slwebtest

# Host test of the kernel syscall layer used by newlib programs (process.c): argv/stack layout,
# fd table, brk, cooked stdin, path calls, exit status, pointer validation. Needs only gcc.
# -DPROCESS_HOST_TEST compiles process.c's syscall layer on any host architecture (its
# privileged x86 instructions are #ifdef __x86_64__ out of the host build).
proctest:
	gcc -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -DPROCESS_HOST_TEST -I. -include string.h -o /tmp/process_host tests/host/process_host.c
	/tmp/process_host

# Host-only unit tests for libslweb (url.c + http.c + html.c + dom.c + layout.c + css.c + image.c
# + png.c) against a scripted, in-memory socket layer: URL parsing, HTTP status/headers,
# Content-Length, chunked, redirects, errors, the DOM tree and exact box geometry, the CSS cascade,
# and PNG/BS64/data: image decoding. The kernel HTML corpus (html_test.py) is reused verbatim.
slwebtest: slwebtlstest
	gcc -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Ilibslweb -o /tmp/slweb_host tests/host/slweb_host.c libslweb/url.c libslweb/html.c libslweb/http.c libslweb/dom.c libslweb/layout.c libslweb/css.c libslweb/image.c libslweb/png.c libslweb/cache.c libslweb/form.c libslweb/tls_user.c tls.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c x509.c
	python3 tests/host/slweb_test.py /tmp/slweb_host
	python3 tests/host/html_test.py /tmp/slweb_host

# Host test of libslweb's HTTPS path over real loopback sockets: the same
# http.c is driven against OpenSSL s_server with an ephemeral CA, checking a
# verified fetch, a certificate that fails to verify, a hostname mismatch and
# --insecure. tls_user.c's tcp_*/krand shims run over real sl_* sockets provided
# by the driver.
slwebtlstest:
	gcc -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Ilibslweb -o /tmp/slweb_tls_host tests/host/slweb_tls_host.c libslweb/url.c libslweb/http.c libslweb/tls_user.c tls.c crypto.c chachapoly.c p256.c bignum.c rsa.c ecdsa.c x509.c
	python3 tests/host/slweb_tls_test.py /tmp/slweb_tls_host

.PHONY: proctest slwebtest slwebtlstest tcptest

# Host test of the kernel TCP (tcp.c) against a mock IP layer: the RFC 7323
# window-scale handshake in both roles, the >64 KiB windows it unlocks, the
# unscaled fallback for a peer that omits the option, and receive-window
# accounting. Needs only gcc.
tcptest:
	gcc -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Itests/host -o /tmp/tcp_host tests/host/tcp_host.c tcp.c
	/tmp/tcp_host
