# StarlightOS 4.0 "Aurora"

*(formerly Myos: build outputs are now `starlightos64.elf` / `starlightos.bin`, the hostname defaults to `starlight`,
and the shell is `starlightsh`.)*

A small x86_64 hobby OS: BIOS boot, long mode, ring-3 ELF apps, ext2, TTF fonts,
a desktop — and, as of 4.0, a **real TCP/IP stack**, an **audio mixer/sequencer**,
and a **modern shell**.

The previous README (v3.4 notes) is kept in [`docs/README_v3.4.md`](docs/README_v3.4.md).

## newlib / hosted C programs

C programs linked against **newlib** (stdio, malloc, math, files, argc/argv, exit codes) run from the shell
like any command. See [`libc/README.md`](libc/README.md): `NEWLIB_SRC=... make newlib`, `make apps/CDEMO.ELF`,
then `CDEMO.ELF a b c` inside StarlightOS. Host test of the kernel side: `make proctest`.

## Roadmap: a ring-3 web browser

The end goal is a static-content web browser (HTML + CSS + images, no JavaScript) that runs as an ordinary
ring-3 ELF app. Work is sliced into milestones, each landing with host tests (`make proctest`) and QEMU
end-to-end checks (`make test`).

| # | Milestone | Status |
|---|---|---|
| M0 | Process groundwork: 64 MiB user window, typed fd table (32 fds), 4 MiB ELF cap, multi-page user mappings | done |
| M1 | **Graphics/input syscalls**: `fbmap`/`present`/`getevents`, input event queue, guest-window compositor | done |
| M2 | **Application network syscalls** (`socket`/`connect`/`send`/`recv`/`poll`/`resolve`) with `kyield()` integration | done |
| M3 | `libslweb`: userland HTTP + URL + HTML parsing lifted out of the kernel | done |
| M4 | **Text-level browser MVP**: fetch a URL, render text, follow links (`BROWSER.ELF`) | done |
| M5 | **DOM + box layout**: parse HTML into a node tree and lay it out into positioned boxes | done |
| M6 | **CSS subset + bitmap images** (`<style>` cascade, colours, box model; PNG + BS64 + `data:`) | done |
| M7 | **Browser chrome**: address bar, back/forward, bookmarks, HTTP cache, HTML forms | done |
| M8 | **Performance: TCP window scaling** (RFC 7323) + a 128 KiB receive window | done |
| M9 | TLS certificate validation (X.509 + RSA/ECDSA) and lifting TLS into the browser | in progress |

### M1: user-mode window syscalls

A ring-3 app owns a fixed-size window through three syscalls (ABI in [`user/slwindow.h`](user/slwindow.h),
dispatcher in `process.c`, compositor in `gui.c`):

* `fbmap(w, h)` allocates a `w*h` `0x00RRGGBB` surface on the kernel heap and maps it read/write (never
  executable) into the process at a fixed address, returning it. The surface is page-aligned and must fit
  the screen (`w+6 ≤ 1920`, `h+39 ≤ 1080`). Mapping the same size again returns the same address; a
  different size is `EBUSY` (resizing comes later).
* `present()` pumps input, composites the desktop, window chrome and the app's pixels, and shows one frame.
* `getevents(buf, count, timeout_ms)` drains up to 64 key/move/down/up/quit events (`0` = poll, `-1` =
  block, `>0` = milliseconds). Ctrl-C aborts a blocked wait with `EINTR`, and the process exits `130`.

While a ring-3 app runs, the desktop loop is suspended and the app drives everything through these calls.
Closing the titlebar delivers `SL_EV_QUIT`; the window is torn down whenever the process exits.
`user/gdemo.c` (`apps/GDEMO.ELF`) exercises the ABI: `make apps/GDEMO.ELF`, then `run GDEMO.ELF`.
`make proctest` checks the dispatcher on the host and `python3 tests/run_tests.py gui` drives it in QEMU.

### M2: application network syscalls

Ring-3 apps get blocking IPv4 TCP sockets on top of the kernel's `tcp.c`, through `user/slnet.h`:

* `int sl_socket(int domain, int type, int protocol)` — `AF_INET`/`SOCK_STREAM` only. The socket is a
  normal process fd (3..31), so `read()`/`write()`/`close()` work on it too.
* `int sl_resolve(const char *host, uint8_t out_ip[4])` — literal IPv4 fast path, then the kernel DNS
  cache/resolver (`dns_resolve`).
* `int sl_connect(int fd, const struct sl_sockaddr_in *addr)` — blocking TCP handshake (10 s cap),
  Ctrl-C aware; maps `TCP_ERR_*` onto `ECONNREFUSED`/`ECONNRESET`/`ETIMEDOUT`/`ENETUNREACH`.
* `long sl_send(int fd, const void *buf, size_t len)` / `long sl_recv(int fd, void *buf, size_t len)` —
  block until the bytes are queued / until data or EOF (30 s cap; a quiet `recv` returns `EAGAIN`).
* `int sl_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)` — the multiplexing primitive. It
  translates process fds to kernel `kfd`s and parks in `kpoll()`, which runs `net_poll()` and
  `kyield()` while it waits, so the NIC — and a ring-3 app's window — stay live. Regular files and
  `stdout` report ready immediately; an unconnected socket is `POLLHUP`.

`process.c`'s fd table gained an `FD_SOCK` slot type wrapping a `kfd`. While a guest window is open,
`gui.c` registers a recursion-guarded `guest_background` poller, so an app blocked in `connect`/`send`/
`recv`/`poll` still gets input and redraws. `user/ndemo.c` (`apps/NDEMO.ELF`) exercises the ABI against
the test HTTP/echo fixtures: `make apps/NDEMO.ELF`, then `run NDEMO.ELF`.
`python3 tests/run_tests.py appnet` drives it in QEMU.

### M3: `libslweb`, a userland web library

The HTTP client, URL parser and HTML->text renderer that used to live only in the kernel
(`url.c`/`http.c`/`html.c`) are now also a ring-3 library, **libslweb**, so the browser can fetch and
render pages from user space on top of the M2 sockets:

* `int slweb_url_parse(const char *url, struct slweb_url *out)` — scheme/host/port/path. `https://`
  sets `->https` (see the HTTPS note below), a schemeless `host/path` defaults to http/80.
* `int slweb_http_request(const char *url, const struct slweb_http_opts *opts, struct slweb_response *res)`
  — blocking GET/HEAD/POST with redirects (`follow_redirects`), `Content-Length`, chunked transfer,
  read-until-close, a per-read `timeout_ms`, a `max_body` cap and an optional progress callback. The
  response exposes status/headers/content-type/location/final URL, the body, and a negative
  `SLWEB_ERR_*` code (`slweb_http_error_text`); free the body with `slweb_http_response_free`.
* `size_t slweb_html_to_text(...)` — the same renderer as the kernel's `fetch --text`: block
  structure, lists, headings, numbered links, entities and `<pre>`, with `<script>`/`<style>`/`<head>`
  skipped.

It is built on newlib (malloc/stdio/string) and `user/slnet.h`; each request does
`sl_resolve`/`sl_socket`/`sl_connect` and waits for readability with `sl_poll`, so a blocked fetch
still pumps the NIC (and any open app window) through the kernel's `poll`/`kyield` path.

**HTTPS is not available yet.** Lifting the TLS client — and validating certificates — is still
ahead (M9); an `https://` URL returns `SLWEB_ERR_HTTPS` (`-12`) so the browser can show an
"unverified connection" badge instead of silently downgrading to cleartext.

Build and use:

```sh
make libslweb                 # builds + installs libslweb.a and <slweb.h> into ./sysroot
make apps/WEBTEST.ELF         # the M3 demo (starlight-gcc adds -lslweb automatically)
# then, in StarlightOS:  run WEBTEST.ELF
```

Host tests: `make slwebtest` runs the URL/HTTP parser against a scripted in-memory socket layer and
re-runs the kernel HTML corpus (`tests/host/html_test.py`) against the lifted renderer.
`python3 tests/run_tests.py web` drives `apps/WEBTEST.ELF` in QEMU against the HTTP fixtures.

### M4: the text-level browser

`user/browser.c` (`apps/BROWSER.ELF`) is the first real browser: a ring-3 app that ties M1 and M3
together. It opens a 900x600 window with `fbmap`, draws the URL bar, the wrapped page text and a
status/hint bar into its own surface with a small built-in 8x8 font, and fetches with libslweb over
the M2 sockets:

```sh
make apps/BROWSER.ELF
# then, in StarlightOS:  run BROWSER.ELF http://10.0.2.2:18001/page
```

* **Fetch + render.** `slweb_http_request` follows redirects; `slweb_html_to_text_ex` renders the
  body and now also returns the collected hrefs (in document order, matching the `[n]` markers).
  Non-HTML content types are copied verbatim.
* **Follow links.** `1`-`9` follow a numbered link, `tab` selects the next one and `enter` follows
  it; clicking a line that carries a `[n]` marker works too. Hrefs are resolved against the current
  URL with the new `slweb_url_resolve`.
* **Navigate.** `b` goes back (a small URL stack), `r` reloads, and `up`/`down`/`pgup`/`pgdn`/`home`/
  `end` scroll (with a proportional scrollbar); `q`/`esc` or the titlebar close control quit.
* **Unverified connections.** `https://` still returns `SLWEB_ERR_HTTPS`; the window shows an
  `[unverified connection]` badge and the error page points at M9.

There is deliberately no address bar yet — that is the M7 chrome milestone — so the URL comes from
the command line and navigation is by following links. A bounded excerpt of the rendered text is
echoed to stdout (`BROWSER: text ...`) so the QEMU test can assert on the render without a pixel
capture. `python3 tests/run_tests.py browse` drives the app: fetch, render, link-follow, back,
reload, exit status, and the https badge.

### M5: DOM + box layout

M4 threw the page structure away as soon as it had wrapped some text. M5 keeps it. `libslweb` gains
two sources, and `BROWSER.ELF` now draws boxes instead of lines:

| File | Job |
|---|---|
| `libslweb/dom.c` | parse HTML into a tree of `struct slweb_node` (elements + decoded text) |
| `libslweb/layout.c` | walk the tree and emit an ordered array of `struct slweb_box` |

* **DOM.** `slweb_dom_parse` builds a document tree: elements carry a lowercased tag and up to 12
  attributes, text nodes carry decoded entities. It is forgiving like a browser — unclosed tags are
  closed at EOF or by their successors (`<li>` closes `<li>`, a block closes `<p>`), orphan close
  tags are ignored, and the contents of `<script>`/`<style>`/`<title>`/`<textarea>`/`<svg>`/
  `<noscript>`/`<template>` are skipped as raw text so a stray `<` in JavaScript can't derail the
  parse. `slweb_dom_from_text` wraps a plain body in a `<pre>` for non-HTML content.
* **Layout.** `slweb_layout(root, viewport_w, boxes, max, links, max_links, &n_links, &doc_w, &doc_h)`
  returns a flat, ordered list of boxes: wrapped text runs, list markers, `<hr>` rules and `<img alt>`
  placeholders (real bitmaps are M6). Block elements stack with margins; inline content flows into a
  shared line and wraps at the right edge; links are handed back in document order (the M4
  `struct slweb_link`) and each box records its 1-based link index for hit-testing. Metrics are fixed
  — 8x8 glyphs in a 10 px line, doubled to 16x16 for `<h1>`/`<h2>` — so the renderer and the tests
  agree on exact geometry.
* **Render.** `user/browser.c` draws each box with the built-in 8x8 font (scaled headings, a faked
  bold weight and an italic shear), underlines links, and hit-tests a click against the boxes. `1`-`9`
  and `tab`+`enter` still follow links; the selected link is highlighted and scrolled into view.
  The text excerpt is now reconstructed from the laid-out boxes, and the browser also logs
  `BROWSER: layout boxes=… doc_h=… links=…`, `BROWSER: link n href=…` and per-link follows.

Host tests: `make slwebtest` now also covers the DOM (`tests/host/slweb_host.c dom`) and layout
(`... layout WIDTH`) against exact box geometry. `python3 tests/run_tests.py browse` asserts the page
is laid out and the link table is built in QEMU.

### M6: CSS subset + bitmap images

M5 laid out structure but ignored presentation. M6 reads the author stylesheet, applies it during
layout, and turns `<img>` into real bitmaps. `libslweb` gains three sources:

| File | Job |
|---|---|
| `libslweb/css.c` | a forgiving CSS subset parser + cascade (`slweb_css_parse`, `slweb_style_compute`) |
| `libslweb/png.c` | a self-contained inflate + baseline PNG decoder (`slweb_png_decode`) |
| `libslweb/image.c` | `data:` URIs, base64, format sniffing, the "BS64" raw bitmap (`slweb_image_decode`) |

* **CSS.** Selectors are type/class/id/universal with descendant and child combinators and
  comma-separated lists; properties cover `color`, `background`, `font-weight`/`font-style`,
  `text-decoration`, `font-size`, `display`, `text-align` and the `margin`/`padding`/`width`/`height`
  box model. `slweb_style_compute` returns only what the author declared (each with a `_set` flag),
  so layout merges it over its defaults and inherits colour, scale and alignment down the tree.
  Inline `style="…"` wins over `<style>` rules; colours accept names, `#rgb`/`#rrggbb` and
  `rgb(r,g,b)`. `<style>` text is now kept in the DOM (only `<script>`/`<title>` etc. stay raw).
* **Images.** `slweb_image_decode` sniffs PNG or the StarlightOS "basesize64" raw format (`BS64`
  magic, version, flags, u16 width/height, then packed RGB/RGBA). PNG support is baseline,
  non-interlaced, including 1/2/4-bit grey and palette. `data:image/png;base64,…` URIs are decoded
  in place, so a page can inline an image with no second request. Pixels are `0xAARRGGBB`; the
  browser scales to the box and alpha-blends.
* **Layout.** `slweb_layout_ex(root, w, opts, ...)` is `slweb_layout` plus `struct slweb_layout_opts
  { css; images[]; n_images }`. Boxes gain author `fg`/`bg`/`underline` and an optional decoded
  `image`; a background emits an `SLWEB_BOX_RECT` behind its content, `text-align` shifts a block's
  lines, and `display:none` drops a subtree. Passing `opts = NULL` is byte-for-byte the M5 path.
* **Render.** `user/browser.c` now fetches and decodes `<img>` sources (inline `data:` URIs and
  fetched PNG/BS64), draws backgrounds first, then text/markers/rules/bitmaps with nearest-neighbour
  scaling and alpha blending, and logs `BROWSER: css rules=… images=…`, `BROWSER: image n src=… w=…
  h=… alpha=…` and `BROWSER: css applied fg=… bg=…` for the QEMU test.

Host tests: `make slwebtest` adds `image` and `css` modes to `tests/host/slweb_host.c` (decode a
PNG/BS64 blob and dump the cascade) and extends `layout` to apply CSS and inline `data:` images.
`python3 tests/run_tests.py browse` now serves a styled page with an inline `data:` PNG and a fetched
`/pic.png` and asserts the compiled rules, decoded images and applied colour.

### M7: browser chrome, cache and forms

M0–M6 could *render* a page but you could not *use* one. M7 gives `BROWSER.ELF` real chrome and adds
two `libslweb` sources:

| File | Job |
|---|---|
| `libslweb/cache.c` | an in-memory, LRU HTTP response cache keyed by absolute URL |
| `libslweb/form.c` | form collection, `application/x-www-form-urlencoded` encoding, submission |

* **Chrome.** The window gains a nav bar (back / forward / reload buttons and an address field) and a
  bookmarks bar. `Ctrl-L` focuses the address bar (the current URL is selected, so typing replaces
  it), `b`/`f` go back/forward, `r` reloads, `Ctrl-D` bookmarks the page and `Ctrl-B` + `1`–`9` opens
  a bookmark. Bookmarks persist to `BOOKMARKS.TXT` (one URL per line) in the current directory and
  are reloaded on the next run. `Tab` cycles focus (form fields first, else links), `Enter`
  activates/submits and `Esc` blurs (or quits when nothing is focused). All of it is keyboard-driven
  for the QEMU test.
* **Cache.** Back/forward and a repeat visit are served from `cache.c` with no second request
  (`BROWSER: cache hit url=…`); a fetch stores the response (`BROWSER: cache store url=…`), and
  reload deliberately bypasses the cache. Entries evict least-recently-used on an entry-count or byte
  budget.
* **Forms.** Layout turns controls into widget boxes (`<input>`/`<textarea>`/`<select>` ->
  `SLWEB_BOX_INPUT`, `<button>`/submit/reset -> `SLWEB_BOX_BUTTON`, each with a `node` back-pointer;
  `hidden` inputs and `option` emit no box). The browser keeps each control's **live** value
  (typed text, checked state, selected option) keyed by node, and on submit collects the successful
  controls with `slweb_form_collect`, encodes them with `slweb_form_query`, and resolves the action
  with `slweb_form_submit` — a GET merges the query into the action URL, a POST sends it as the body.
* **Render.** `user/browser.c` draws the toolbar, address field, bookmark bar, form widgets, and a
  status line, and logs `BROWSER: addr focus`, `BROWSER: go …`, `BROWSER: back -> …`,
  `BROWSER: forward -> …`, `BROWSER: reload url=…`, `cache hit`/`cache store`, bookmark add/open, and
  `BROWSER: submit method=… fields=… url=…` for the QEMU test.

Host tests: `make slwebtest` adds `cache` (put/get/clear + LRU and byte-budget eviction) and `form`
modes to `tests/host/slweb_host.c`, plus form-control `layout` boxes (`el=<tag> name=… type=…`).
`python3 tests/run_tests.py browse` now drives the M7 browser end to end: type a URL into the address
bar, go back/forward from the cache, add a bookmark, restart the browser and reopen the bookmark from
`BOOKMARKS.TXT`, and load a form page, edit a text field and checkbox, and submit it (GET) to a
server endpoint (and it checks the saved bookmark on the disk image).

server endpoint (and it checks the saved bookmark on the disk image).

### M8: TCP window scaling

The kernel TCP stack (`tcp.c`) used to advertise a fixed 16 KiB window and ignore
the peer's, so a single connection could never keep much data in flight. M8
negotiates **RFC 7323 window scaling** on every connection and enlarges the
receive buffer:

* **Options.** Our SYN carries the MSS option plus a window-scale option (kind 3);
  our SYN-ACK echoes the window-scale option only when the peer sent one. The
  scale we advertise is the smallest shift for which the whole receive buffer
  fits in the 16-bit window field (2 for the 128 KiB buffer).
* **Scaling both directions.** The window field in the SYN and SYN-ACK is never
  scaled (per the RFC). After the handshake the window we accept from the peer is
  `wnd << snd_wscale`, and the window we advertise is `free_rx >> rcv_shift`. A
  peer that omits the option leaves both directions unscaled, so the advertised
  window is capped at 65535 — correct interoperation with older stacks.
* **Bigger buffers.** `RX_BUF` is 128 KiB (was 16 KiB) and `TX_BUF` 64 KiB, and
  the congestion-window ceiling is raised to the receive buffer so the scaled
  window can actually be filled. Even against a peer that never scales (QEMU's
  libslirp, for instance), the connection now advertises up to 64 KiB — four
  times more than before.

`netstat` gains a `WS` column (`snd/rcv` scale per socket) and `netstat -s`
counts window-scaled connections.

Host tests: `make tcptest` compiles `tcp.c` against a mock IP layer and drives the
negotiation directly — the SYN/SYN-ACK options, the >64 KiB advertised window and
scaled peer window when the peer scales, the unscaled fallback when it does not,
and the receive window shrinking as unread data arrives. The QEMU `net` group
still fetches a 300 KB body byte-for-byte to exercise the larger window.

## Quick start

```sh
# macOS (Homebrew):  brew install i686-elf-gcc x86_64-elf-gcc qemu
# Linux: x86_64-elf-gcc / i686-elf-gcc cross compilers + qemu-system-x86 + python3
make run                      # build disk.img and boot it
make run QEMU_AUDIO=pa        # pick the host audio backend: pa | alsa | coreaudio | dsound | sdl
make test                     # headless end-to-end tests (needs qemu + python3; numpy for audio checks)
```

`make run` gives the VM an RTL8139 NIC (QEMU user-mode NAT, so `10.0.2.2` is your host)
and an AC'97 sound card. Open the terminal from the dock (or press `6`); type `help` for commands and `neofetch` for a system summary.

## Added after 4.0: TrueColor, HTTPS, telnet, tighter terminal

* **TrueColor** (`gfx.h`, `vga.c`, `vbe.c`, `starlightos_gui.*`, `gui.c`): the display now runs in a 32-bit VBE mode
  (1920x1080x32) and every surface is `gfx_t` = `0x00RRGGBB`. Colours are plain 24-bit values everywhere
  (`COL_ACCENT` is exactly `0x3584E4`, not a 6-bit approximation); `RGB(r,g,b)`, `gfx_mix()` and `gfx_shade()` do
  integer blends (no SSE/float in the kernel). What changed visibly: a smooth dithered 24-bit wallpaper with a
  radial sun glow and anti-aliased, fog-shaded mountains; real alpha-blended soft window/dock/toast shadows;
  anti-aliased rounded corners; a translucent dock; gradient title bars; true anti-aliased text (glyph coverage
  blended over whatever is behind it instead of snapped to the nearest palette entry); a modern 16-colour terminal
  palette (`gfx_vga16`, GNOME terminal colours; attributes are still the 4-bit 0-15 values the shell uses).
  New primitives: `vga_blend_pixel`, `vga_fill_rect_alpha`, `vga_fill_gradient_v`. If no 32 bpp VBE mode is available
  the kernel stays in 320x200 mode 13h and quantises to a 3-3-2 colour cube when presenting (same code, fewer colours).
  Cost: three 1920x1080 surfaces are ~25 MB of `.bss` (was ~6 MB); fine with the default `-m 256M`.
  `make shots` renders the real desktop code on the host to PPM files so you can see it without QEMU.
* **`/dev` device filesystem** (`devfs.c devfs.h`): a real unix-style device directory at the root
  of the disk. `make_disk.py` creates the `/dev` directory on the ext2 volume (so `ls /`, `tree` and
  the desktop file window see it), and the VFS mounts the synthetic devfs over it at boot — the mount
  table picks the most specific match, so everything under `/dev` reaches the drivers while the rest
  stays on ext2. The nodes are wired straight into the kernel: `/dev/null` (reads end-of-file, writes
  discarded), `/dev/zero` (endless zeroes), `/dev/random` and `/dev/urandom` (bytes from the krand
  entropy pool), and `/dev/tty` (reads block for keyboard input, writes go to the console). Because
  they are ordinary VFS paths, `cat`, `hexdump`, pipelines, `>` redirects and `rm` all work on them
  with the expected semantics — `echo x > /dev/null` succeeds and drops the data, `rm /dev/null` is
  refused with "operation not permitted", and `cat /dev/zero` streams until Ctrl-C, like unix.
* **fetch fix**: pages that rendered as *nothing* (just "200 OK") are fixed. The HTML renderer treated `<script>`
  contents as markup, so a stray `<` or quote in JavaScript, or a `<head>` that is never closed, swallowed the whole
  page. Script/style/noscript/svg/template/title are now skipped as raw text up to their real close tag, `<head>`
  ends at `<body>` (or the first non-head tag), and quotes only count inside tags after `=`. A page that truly has no
  text (JavaScript-only sites) now says so and points at `fetch -r`. Regression + fuzz tests: `make hosttest`.

* **HTTPS** (`tls.c crypto.c chachapoly.c p256.c krand.c`): `fetch https://...` / `curl` / `wget`.
  - **Versions:** TLS 1.3 and TLS 1.2 (ECDHE, with extended master secret when the server offers it).
  - **Key exchange:** X25519 and P-256 (secp256r1). Both key shares are sent up front, so a P-256-only server
    (Windows/IIS, older Java, some load balancers) needs no extra round trip.
  - **Ciphers:** ChaCha20-Poly1305 (offered first; several times faster than our table-less AES on a CPU
    without AES-NI), AES-128-GCM, AES-256-GCM (SHA-256 / SHA-384 key schedules). TLS 1.2 uses the
    ECDHE-{RSA,ECDSA} variants of the same three.
  - **Interop details that matter on the real internet:** SNI, a ClientHello padded past the old F5 256-511 byte bug,
    servers that put ServerHello + Certificate + ServerHelloDone in ONE record (Go, Java, Schannel),
    CertificateRequest answered with an empty certificate list, KeyUpdate, close_notify, downgrade-sentinel check.
  - **`fetch -v`** prints every handshake step: what we offered, what the server picked, and, on failure, which TLS
    alert it sent (by name), so "doesn't work on site X" becomes a specific, fixable message.
  - **Tests:** `make hosttest` (primitives vs OpenSSL/hashlib: 1700+ cases, X.509 parsing/validation: 47 cases, plus
    telnet/HTML) and `make tlstest` (the TLS client against OpenSSL servers in 40+ configurations, 300 KB transfers,
    coalesced records, certificate and hostname validation, and a fuzzer that corrupts the server's flight under
    ASan/UBSan; needs `openssl` and python3).
  - **Certificate validation:** the chain is checked against a trust store (signature chain up to a root, validity
    window, DNS subjectAltName/CN with single-label wildcards) and the CertificateVerify (1.3) / ServerKeyExchange (1.2)
    signature is verified with the leaf's key (`x509.c`, shared with libslweb). Accepts RSA (PKCS#1 v1.5 and PSS) and
    ECDSA (P-256/P-384) certificates. Verification is **on by default and fails closed**: the trust store starts empty,
    so every chain is rejected until you add roots with `fetch --cafile FILE` (a PEM bundle); `-k`/`--insecure` accepts
    any certificate. Also unsupported: HelloRetryRequest, client certificates, session resumption, TLS 1.1 and older,
    static-RSA key exchange, Ed25519 certificates, and revocation (CRL/OCSP). Entropy for key generation is TSC jitter +
    clock + MAC (+ RDRAND when the CPU has it); fine for a hobby OS, not a certified CSPRNG. P-256 is not constant-time
    (ephemeral keys only).
* **`telnet HOST [PORT]`** (`telnet.c`, `cmd_net.c`): option negotiation (ECHO, SGA, TTYPE="DUMB", NAWS), line mode
  with local echo until the server says WILL ECHO, then character mode with arrow/Home/End/Del keys. ANSI escapes
  from the server are stripped. Ctrl-C closes the connection. `telnet host 80` also works as an HTTP poke tool.
* **Terminal spacing**: character cell narrowed from 12 to 9 px (`TERMINAL_CHAR_W` in `terminal.h`; JetBrains Mono
  ink is at most 8 px wide, so there is still a 1 px gutter). The default 80-column window is now 722 px wide
  instead of 962. If it looks too tight, set it to 10.

## Added after 4.0: a more Unix-like shell and userland

Everything below is exercised by `make shelltest` (85 checks, run under ASan/UBSan, using the real shell, VFS, ext2,
devfs and procfs code on the host with only the hardware stubbed: `tests/host/shell_host.c`, `shell_test.py`).

* **Operators**: `a && b`, `a || b`, `! cmd`, `;`, pipes, `<`, `>`, `>>`, plus **stderr**: `2>file`, `2>>file`, `2>&1`, `&>file`,
  `>&2`. Error messages now go to the screen even inside a pipe (`cat nofile | wc -l` prints the error and `0`) instead of being
  piped into the next command; `2>/dev/null` silences them.
* **Expansions**: `$(cmd)` and `` `cmd` `` (nestable, quote-aware), **globbing** (`*`, `?`, `[a-c]`, `[!x]` in the last path
  component, sorted, dotfiles only when the pattern starts with `.`), `${VAR:-default}`, `${#VAR}`, `$#`, `$@`, `"$@"`, `$*`, `$$`,
  `$0..$9`, arithmetic on variables (`$((n*2+1))`), `# comments` anywhere, `NAME=value` assignments and `NAME=value cmd`.
* **Control flow** (one-liners with `;` and script files): `if/then/elif/else/fi`, `while`/`until`/`for X in WORDS`/`do`/`done`,
  `break`, `continue`, `exit [N]`, `return [N]`, `shift`, `read [-p PROMPT] [NAME...]`, `eval`, `true`, `false`, `:`, `type`,
  `.`; loops can read input with `done < file` or `producer | while read line; do ...; done`.
* **Scripts are real programs**: an executable file in `$PATH` (or `./x.sh`) runs with arguments (`$1 $2 ...`), returns its exit
  status (`$?`, `&&`, `||`), and its output can be piped or redirected (`./x.sh | grep y > out`). `source FILE ARGS` and `sh FILE ARGS` too.
* **New commands** (`cmd_posix.c`): `diff` `cmp` `sed` (`s/old/new/[g]`, `Nd`, `N,Mp`, `$`, `-n`; literal patterns, no regex engine)
  `rev` `tac` `nl` `fold` `paste` `file` `readlink` `realpath` `mktemp [-d]` `chgrp` `printenv` `mount` `ps` `su [USER]`
  (switches uid/gid/HOME from `/etc/passwd`; there are no passwords). `which` now searches `$PATH`.
* **/proc**: `cpuinfo`, `mounts`, `filesystems` join `version uptime meminfo hostname`.
* **ls** lists files given on the command line (`ls /etc/passwd`, `ls -l a b`), takes several arguments, hides dotfiles unless `-a`,
  and prints one name per line when piped (`ls | wc -l` counts files) or with `-1`.
* **cat** is byte-exact like unix cat: it no longer invents a trailing newline (so `cat a > b` is an exact copy and `cat f | wc -c`
  is right). At an interactive prompt a final line without a newline is still terminated so the prompt starts on a fresh line.
* `sample`: `disk_files/UNIXDEMO.SH` (`source UNIXDEMO.SH alpha`) tours the new features.
* Limits that grew: 48 variables of up to 255 chars, 63 arguments per command, 4 KiB token buffer, nested `$( )` output up to 16 KiB.

### Bugs fixed along the way

* **`/proc` stat wrote to address 0**: `proc_op_stat` sized a file by "reading" it into a NULL buffer, and `memcpy(NULL, ...)` scribbled
  the rendered text over physical page 0 every time a `/proc` file was `stat`ed or `cat`ed (found by the host harness).
* A backslash before `$` inside double quotes (`"\$HOME"`) still expanded the variable.
* `cmd > file` in a command that itself runs a script/`source`/`xargs` used a file name that had been overwritten by the nested
  command line; redirect targets and arguments are now private copies.
* `command < file` leaked the file's contents (it was never freed).
* Scripts always reported status 0; now the status is the script's last command (or its `exit N`).

## What's new in 4.0

### Networking (`net.c tcp.c dns.c http.c html.c url.c`, `rtl8139.c`)

| Layer | Details |
|---|---|
| NIC | RTL8139 driver rewritten: correct RX ring wrap (the old `WRAP` bit + masked reads corrupted frames that crossed the ring end), TX-slot tracking, link status, error/overflow counters, receiver resync |
| ARP | cache with ageing, answers ARP requests, resolves before timing ping |
| IPv4 / ICMP | checksums verified, echo reply + request, TTL control, TTL-exceeded / unreachable parsing (used by `traceroute`) |
| UDP | sockets with receive queues |
| **TCP** | active **and passive** open, MSS option, in-order receive with dup-ACK on gaps, advertised window, send buffer, **retransmission with RTT-estimated RTO** (Karn, backoff), slow-start cwnd, graceful FIN close, TIME_WAIT, RST handling |
| DHCP | **non-blocking** client in the background: retries, NAK handling, lease renewal; reads router, DNS, NTP, domain |
| DNS | A records, **CNAME chains**, name compression, TTL cache, server failover, NXDOMAIN |
| NTP | SNTP client, can set the RTC |
| **HTTP/1.1 + HTTPS** | redirects (relative, absolute, protocol-relative; loop guard), **chunked** transfer, Content-Length / read-until-close, HEAD/POST, custom headers, size cap |
| HTML | HTML → text renderer (headings, lists, links with numbered footnotes, entities, skips `<script>`/`<style>`) |

Everything is polled from `kyield()`, which every waiting loop (and the desktop idle loop) calls,
so no network IRQ is required and nothing blocks the machine.

### Sound (`ac97.c audio.c`, `cmd_sound.c`)

* **AC'97 driver**: 32-buffer DMA ring kept topped up from the timer tick (no more one-shot blocking tone), automatic stop after idle, underrun recovery.
* **Software mixer**: 8 voices, sine/square/saw/triangle/noise, attack/release envelopes, soft limiter, master volume, live L/R peak meters (shown in the top bar).
* **MML sequencer**: up to 3 simultaneous music tracks + a sound-effect track, per-track tempo, ties, dotted notes, loops.
* **WAV player**: 8/16-bit PCM, mono/stereo, any sample rate (linear resampling to 48 kHz).
* **Offline renderer**: `synth` writes any melody to a 24 kHz WAV on disk.
* UI sounds (startup chime, notifications) — `sound ui off` to silence. PC speaker fallback when no AC'97 is present.

### Shell (`shell.c cmd_*.c`)

Quoting, `$VAR`, `$(cmd)`, globs, `;` `&&` `||`, **pipes** `|`, **redirection** `>` `>>` `<` `2>`, `if`/`while`/`for`, aliases, `AUTOEXEC.SH` / `source`,
cursor-key line editing, history, **tab completion** (commands and files), `Ctrl-A/E/U/K/W/L/C`,
**1000-line scrollback** (PgUp/PgDn), colour prompt, `$?` exit status.

```
Network  ip ping traceroute nslookup arp netstat ntpdate fetch nc telnet httpd
Sound    sound beep tone play wav aplay synth vol
Text     head tail grep wc sort uniq more hexdump seq        Files  ls cat cp mv rm mkdir cd tree find df ...
System   neofetch cpuinfo lspci mem uptime date cal tz hostname ...        Shell  alias set source calc notify help
```

### `fetch` (alias `curl`, `wget`)

```
fetch http://10.0.2.2:8000/                 # status line + body (HTML is rendered as text)
fetch -o PAGE.HTM URL      fetch -O URL     # save (progress shown); -O picks an 8.3 name from the URL
fetch -i URL   fetch -I URL   fetch -v URL # headers / HEAD / verbose request+response
fetch -d "a=1&b=2" -H "X-Key: v" URL       # POST with headers
fetch -t URL | -r URL                       # force text rendering / raw body
fetch -L URL   fetch --no-redirect URL      # follow redirects (default 5 hops) / don't follow
fetch -X PUT -d "x=1" URL                   # choose the method (-d alone implies POST)
fetch -m 10 --max 65536 -q URL              # 10 s timeout, body size cap, quiet (no status line)
fetch -k https://host/                      # skip certificate validation (accept any certificate)
fetch --cafile roots.pem https://host/      # validate the server chain against the given PEM roots
fetch URL | grep title | wc -l              # raw body flows through pipes
```

### `sound`

```
sound                    status, codec, stream, live meters      sound test      speaker sweep
play twinkle             built-in songs: scale twinkle ode elise arp march
play -w saw "t140 o4 l8 c e g >c, o2 c2 g2"     two tracks      play -f SPLASH.MML -l     loop a file
wav CHIME.WAV            tone 440 1000 square    beep            vol 60 | vol up | sound mute
synth OUT.WAV twinkle    render to a WAV file    sound stop | pause | resume | ui off | meter
```
MML: `a-g` notes (`+`/`-` sharp/flat), `r` rest, `o4` octave, `< >` octave down/up, `l8` default length,
`t120` tempo, `v0-15` volume, `w0-4` waveform, `4.` dotted, `&` tie, `,` starts another track.

### Other

* `httpd [PORT]` serves the current directory; `nc HOST PORT` / `nc -l PORT`.
* Heap allocator (`kheap`, 40 MiB NOLOAD arena), `kprintf`, a real `string.h` subset, COM1 serial console mirroring the terminal, TSC-calibrated microsecond clock, `ktime` calendar/timezone.
* Desktop: live **network state/IP**, **volume + level meter** in the top bar; toast notifications (`notify TEXT`, DHCP connect, volume).
* Disk is now **16 MiB** with room for a 512 KiB kernel and 256 root entries. `make_disk.py` ships sample content: `CHIME.WAV`, `STEREO.WAV`, `TONE8.WAV`, `.MML` songs, `INDEX.HTM`, `DEMO.SH`, `AUTOEXEC.SH` (everything in `disk_files/` is copied in).

### Bugs fixed in existing code

* `vfs.c` `path_parent()` **looped forever** on any `dir/file` path (`rm DIR/X`, `mkdir A/B`, `mv`, writes) — loop condition fixed.
* Filesystem lookups went to the disk for every block, making allocation O(n²): a 300 KB file took **56 s** to write; now a block cache + next-fit hint makes it **~2 s**.
* RTL8139 RX `WRAP` misconfiguration (see above); `fetch` previously failed at DHCP on every boot.
* Commands ran inside the keyboard IRQ with interrupts off, so `beep` could hang the machine; keys are now queued by the IRQ and executed from the main loop.

## Trying the network

```sh
python3 -m http.server 8000 &        # on your host
# in StarlightOS:
ip                                    # lease from QEMU's DHCP: 10.0.2.15, gw 10.0.2.2, dns 10.0.2.3
fetch http://10.0.2.2:8000/ -t        # your host, via QEMU's NAT
ping -c 3 10.0.2.2
nslookup example.com                  # uses 10.0.2.3 -> your host's resolver
ntpdate -s                            # set the clock from pool.ntp.org
httpd 8080                            # then, with  QEMU_EXTRA_ARGS='-netdev user,id=net0,hostfwd=tcp::8080-:8080'
                                      # browse http://localhost:8080/ from the host
```

## Tests

`tests/run_tests.py` boots the real disk image headlessly, types into the shell over QMP, and checks:
shell semantics, file contents **read back from the disk image**, TCP/HTTP behaviour against local
fixture servers (redirects, chunked, 300 KB byte-exact downloads, POST, `httpd` serving a real host client),
DNS (CNAME + compression + NXDOMAIN) and NTP against local stub servers, and **audio captured from QEMU and
FFT-analysed** (pitch, duration, waveform harmonics, stereo separation, resampling, volume scaling, clicks, clipping).

## Layout

```
kernel.c      terminal, scrollback, boot               shell.c cmd_text.c cmd_unix.c cmd_posix.c cmd_net.c cmd_sound.c   shell + commands
net.c tcp.c dns.c http.c html.c url.c rtl8139.c        ac97.c audio.c                             network / sound
kstring.c kprintf.c kheap.c ktime.c ksched.c serial.c   libc-ish kernel services
gui.c starlightos_gui.c vga.c vbe.c font*.c                    desktop (TrueColor, 1920x1080 VBE)
vfs.c ext2.c devfs.c disk.c pci.c pit.c keyboard.c mouse.c ...  drivers
x86_64_port/  user/  apps/  make_disk.py  disk_files/  tests/
```

## Known limitations (honest list)

* **Shell**: no functions, `case`, here-documents, background jobs (`cmd &` is not supported: the OS is cooperative), or multi-line
  input at the interactive prompt (use `;` on one line, or put it in a script). Control flow inside `&&`/`||` chains is not supported.
  `sed`/`grep` patterns are literal text (no regex engine yet). `ps` shows only the shell and itself (no real process table).
  Not yet tested in QEMU or on the cross toolchain: the new code was compiled with gcc `-m64`/`-m32` `-Wall -Wextra` and run on the host.

* **TLS certificate validation is new and minimal** (TLS 1.3 and 1.2; x25519/P-256, RSA/ECDSA certificates, no revocation
  checks). The trust store starts empty, so pass `--cafile FILE` (or `-k`) until the built-in CA bundle lands in M9.5.
* IPv4 only; no IP fragmentation; one NIC model (RTL8139), one sound model (AC'97, 48 kHz out only — no recording).
* TCP has no SACK and no out-of-order receive queue (a gap is answered with a duplicate ACK), so loss recovery is basic; RFC 7323 window scaling (M8) lifts the single-connection window to 128 KiB.
* Cooperative multitasking only (no preemptive scheduler); a command that never yields freezes the UI.
* Tested on QEMU 8.2 only, never on real hardware. Internet access was not available while developing, so DNS/NTP were verified against local stub servers and HTTP against local servers (all through QEMU's NAT), not the public internet.
* The desktop is TrueColor (32-bit VBE), but it is still redrawn by the CPU into software surfaces (no GPU acceleration, no real window compositor); if no 32 bpp VBE mode exists it falls back to 320x200 mode 13h.
