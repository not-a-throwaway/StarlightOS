# newlib on StarlightOS

Hosted C programs (stdio, malloc, string, math, time, ...) running in ring 3 on StarlightOS,
launched from the shell like any other command.

## How it fits together

| Piece | File | Job |
|---|---|---|
| newlib | *(external source)* | the C library itself, built for `x86_64-elf` without libgloss |
| syscall glue | `libc/syscalls.c` | `_write`, `_read`, `_open`, `_sbrk`, `_fstat`, ... → `int $0x80` |
| startup | `libc/crt0.S` | `_start`: reads argc/argv from the stack, runs `main`, `exit()` |
| link layout | `libc/starlight.ld` | the segment layout the kernel loader expects at `0x40000000` |
| compiler driver | `libc/starlight-gcc` | `x86_64-elf-gcc` with the right flags, libs and script |
| kernel side | `process.c` | fd table, brk heap, cooked stdin, argv, exit status, cwd/time/path calls |

## Build

```sh
# 1. one-time: get the newlib source (release tarball or git) and build it into ./sysroot
NEWLIB_SRC=/path/to/newlib-4.x make newlib

# 2. build the demo and put it on the disk
make apps/CDEMO.ELF        # any apps/*.ELF is copied to / on disk.img by make_disk.py
make run
```

Your own program:

```sh
sysroot/bin/starlight-gcc -O2 myprog.c -o apps/MYPROG.ELF
make run
```

## From the StarlightOS shell

```
CDEMO.ELF                    # libc self-test (printf, malloc, qsort, math, files, time)
CDEMO.ELF one "two words"    # argc / argv
CDEMO.ELF -i                 # interactive stdin (line editing, Ctrl-D = EOF, Ctrl-C = abort)
CDEMO.ELF -x 7 ; echo $?     # exit status reaches the shell  -> 7
CDEMO.ELF -cat HELLO.TXT     # file I/O via open()/read()
run CDEMO.ELF a b c          # the explicit form
```

## Kernel ABI (`int $0x80`, rax = number, args rdi/rsi/rdx, result in rax, `-errno` on error)

`0 read` `1 write` `2 readchar (legacy)` `3 close` `4 stat` `5 fstat` `8 lseek` `12 brk` `16 isatty`
`39 getpid` `60 exit` `74 fsync` `79 getcwd` `80 chdir` `82 rename` `83 mkdir` `84 rmdir`
`87 unlink` `96 gettimeofday` `100 uptime_us` `101 open`
`110 fbmap` `111 present` `112 getevents` (ring-3 window, `user/slwindow.h`).
`120 socket` `121 connect` `122 send` `123 recv` `124 poll` `125 resolve` (ring-3 network, `user/slnet.h`).

### Window syscalls

A hosted app can open a window and draw into it (see `user/slwindow.h` for the event struct):

* `void *sl_fbmap(int w, int h)` — allocate a `w*h` `0x00RRGGBB` surface, map it read/write into the
  process and return its address (`NULL` + `errno` on failure). Fixed size for now.
* `int sl_present(void)` — composite the desktop and the app's pixels, present one frame.
* `int sl_getevents(struct sl_event *buf, int count, int timeout_ms)` — key/mouse/close events
  (`0` = poll, `-1` = block, `>0` = milliseconds).

`apps/GDEMO.ELF` (`user/gdemo.c`) is the demo: `make apps/GDEMO.ELF`, then `run GDEMO.ELF`.

### Application network syscalls

Blocking IPv4 TCP sockets (see `user/slnet.h` for the address/poll structs):

* `int sl_socket(int domain, int type, int protocol)` — `AF_INET`/`SOCK_STREAM`; the fd also works with
  `read`/`write`/`close`.
* `int sl_resolve(const char *host, uint8_t out_ip[4])` — literal IP or DNS name.
* `int sl_connect(int fd, const struct sl_sockaddr_in *addr)` — blocking handshake.
* `long sl_send(int fd, const void *buf, size_t len)` / `long sl_recv(int fd, void *buf, size_t len)`.
* `int sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)` — wait for readiness; pumps the NIC
  (and any open app window) while it waits.

`apps/NDEMO.ELF` (`user/ndemo.c`) is the demo: `make apps/NDEMO.ELF`, then `run NDEMO.ELF`.

### The userland browser library (`libslweb`)

[`../libslweb/`](../libslweb/) is a ring-3 library built on this newlib port: URL parsing
(`slweb_url_parse`), a blocking HTTP/1.1 client (`slweb_http_request`) and an HTML->text renderer
(`slweb_html_to_text`), the M3 lift of the kernel's `url.c`/`http.c`/`html.c` onto the `sl_*` sockets.
`make libslweb` installs `libslweb.a` and `<slweb.h>`; `starlight-gcc` then links `-lslweb`
automatically, so a program only needs `#include <slweb.h>`. Host test: `make slwebtest`.

`apps/BROWSER.ELF` (`user/browser.c`, the M4 milestone) is the first consumer: a text-level browser
that draws into an M1 window and follows links resolved with `slweb_url_resolve`.

## Limits (by design of the current kernel)

* One foreground process, no fork/exec/wait/pipes/signals (`fork` etc. return `ENOSYS`).
* Image + heap share a 64 MiB window, stack is 256 KiB, ELF file ≤ 4 MiB, 29 open files.
* Files opened for writing are buffered in kernel memory (≤ 1 MiB) and written back on
  `close()`, `fsync()` or process exit, because the VFS only supports whole-file writes.
* stdin is line-buffered with echo; there is no raw/non-blocking mode and no `termios`.
* No environment variables (`environ` is empty).

## Status

`make proctest` (host) covers the kernel syscall layer; `libc/starlight.ld` + `crt0.S` +
`syscalls.c` were linked with a stub libc and checked against the loader's ELF rules.
The newlib build and a real boot under QEMU were **not** run when this was written (no
network or cross-compiler in that environment) — expect to fix small things on first build.
