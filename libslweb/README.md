# libslweb

The userland half of the StarlightOS browser: URL parsing, a blocking HTTP/1.1 client, an
HTML -> text renderer, and (M5) an HTML DOM + box-layout engine, extended in M6 with a CSS subset
and bitmap images and in M7 with an HTTP cache and form handling, for ring-3 programs linked
against newlib.

It is the M3 lift of the kernel's `url.c` / `http.c` / `html.c`: the same algorithms, but instead of
the kernel's TCP + TLS layers they drive the M2 application network syscalls (`user/slnet.h`) and use
newlib for malloc/stdio/string. M5 adds `dom.c`/`layout.c` on top; M6 adds `css.c` / `image.c` /
`png.c` and a style/image-aware layout entry point; M7 adds `cache.c` / `form.c`.

| File | Job |
|---|---|
| `slweb.h` | the public API (`slweb_url_parse`, `slweb_http_request`, `slweb_html_to_text`, `slweb_dom_parse`, `slweb_layout`, `slweb_layout_ex`, `slweb_css_parse`, `slweb_image_decode`, ...) |
| `url.c` | `http://host:port/path`, https detection, IPv6 rejection, relative-link resolution |
| `http.c` | GET/HEAD/POST, redirects, Content-Length, chunked, read-until-close, errors |
| `html.c` | HTML -> wrapped plain text (block tags, lists, links, entities) |
| `dom.c` | M5: HTML -> a `struct slweb_node` tree (elements, attributes, decoded text) |
| `layout.c` | M5/M6/M7: DOM + viewport width (+ CSS + images + form widgets) -> `struct slweb_box` |
| `css.c` | M6: CSS subset parser + cascade (selectors, colours, box model, text properties) |
| `image.c` | M6: `data:` URIs, base64, format sniffing, and the "BS64" raw bitmap decoder |
| `png.c` | M6: a self-contained inflate + baseline PNG decoder |
| `cache.c` | M7: an in-memory, LRU HTTP response cache keyed by URL |
| `form.c` | M7: form collection, `application/x-www-form-urlencoded` encoding and submission |

## Build

```sh
make libslweb                 # build/ + install libslweb.a and <slweb.h> into ../sysroot
make apps/WEBTEST.ELF         # the demo; starlight-gcc links -lslweb automatically
```

`starlight-gcc` appends `-lslweb` only when the archive is installed, so programs that don't use the
library are unaffected.

## Use

```c
#include <slweb.h>

struct slweb_http_opts o = { 0 };
o.follow_redirects = 5;
struct slweb_response r;
int err = slweb_http_request("http://example.com/", &o, &r);
if (err == SLWEB_OK) {
    char text[8192];
    slweb_html_to_text((const char *)r.body, r.body_len, text, sizeof text, 78, 1);
    printf("%s\n", text);
}
slweb_http_response_free(&r);
```

* Every request is blocking. It runs `sl_poll` while it waits, so the NIC — and, when a window is
  open, the app's compositor — stay responsive.
* `slweb_http_request` returns `0` or a negative `SLWEB_ERR_*` code; `res->status` still holds the
  HTTP status when a valid head arrived before an error (e.g. a truncated body).
* The body is `malloc`'d and NUL-terminated; free it with `slweb_http_response_free`.
* **HTTPS is not implemented yet** (`SLWEB_ERR_HTTPS`, `-12`): the userland TLS client and
  certificate validation are the M9 milestone.

### Following links

`slweb_html_to_text_ex` is the same renderer but also copies the collected hrefs (document order,
matching the `[n]` markers it prints) into a caller buffer, and `slweb_url_resolve` turns a relative
href into an absolute URL against the page it was found on:

```c
struct slweb_link links[SLWEB_MAX_LINKS];
size_t n;
slweb_html_to_text_ex(body, len, text, sizeof text, 78, 1, links, SLWEB_MAX_LINKS, &n);

char target[512];
slweb_url_resolve("http://example.com/a/b/", links[0].href, target, sizeof target);
/* "/x" -> http://example.com/a/x, "c" -> http://example.com/a/b/c, ... */
```

`apps/BROWSER.ELF` used this text path in M4; since M5 it renders from the DOM + box layout below
(the text renderer is still what `fetch`/`WEBTEST` use).

### DOM + box layout (M5)

The browser needs structure, so the library also parses HTML into a DOM and lays it out:

```c
struct slweb_node *dom = slweb_dom_parse(body, len);   /* or slweb_dom_from_text() */

struct slweb_box boxes[SLWEB_MAX_BOXES];
struct slweb_link links[SLWEB_MAX_LINKS];
size_t nlinks; int doc_w, doc_h;
int nboxes = slweb_layout(dom, viewport_w, boxes, SLWEB_MAX_BOXES,
                          links, SLWEB_MAX_LINKS, &nlinks, &doc_w, &doc_h);

for (int i = 0; i < nboxes; i++) draw(boxes[i]);        /* text/rule/image/marker */
slweb_dom_free(dom);
```

* `slweb_dom_parse` is forgiving: implicit closes (`<li>`/`<p>`/table cells), void elements,
  attributes, entity decoding, and raw-text elements (`<script>`/`<style>`/`<title>`, ...).
  `slweb_node_attr(node, "href")` looks up an attribute.
* `slweb_layout` emits an ordered list of `struct slweb_box`: kind (`TEXT`/`IMAGE`/`RULE`/`MARKER`),
  rectangle `x,y,w,h`, glyph `scale`/`bold`/`italic`, a 1-based `link` index, and the text slice
  (pointing into the DOM). Metrics are fixed: 8x8 in a 10 px line, 16x16 for `<h1>`/`<h2>`.
* Links come back in document order in `links[]`, so `box.link` indexes it exactly like M4's
  numbered links; `slweb_url_resolve` resolves the href against the page URL.

`slweb_dom_free` releases a tree. `apps/BROWSER.ELF` (`user/browser.c`, M6) draws these boxes with
its 8x8 font, applies the author stylesheet and images below, and hit-tests clicks against them.

### CSS subset + bitmap images (M6)

M6 adds a small, forgiving CSS 2.1 subset and image decoding, wired into layout through
`slweb_layout_ex` (plain `slweb_layout` is a wrapper that passes no styles/images, so the M5
behaviour is unchanged):

```c
struct slweb_css *css = slweb_css_parse(dom);          /* collects <style> blocks */
struct slweb_image_entry imgs[8];
size_t n_imgs = decode_inline_images(dom, imgs, 8);    /* data: URIs, or fetched PNG/BS64 */

struct slweb_layout_opts opts = { css, imgs, n_imgs };
int nboxes = slweb_layout_ex(dom, viewport_w, &opts, boxes, SLWEB_MAX_BOXES,
                             links, SLWEB_MAX_LINKS, &nlinks, &doc_w, &doc_h);
slweb_css_free(css);
```

* **Selectors**: type (`p`), class (`.c`), id (`#i`), universal (`*`), descendant (whitespace) and child
  (`>`) combinators, and comma-separated lists. **Properties**: `color`, `background(-color)`,
  `font-weight`, `font-style`, `text-decoration`, `font-size`, `display`, `text-align`, the
  `margin`/`padding` shorthands and longhands, `width`, `height`.
* `slweb_style_compute` fills a `struct slweb_style` with **only** the declarations the author gave,
  each with a `_set` flag, so layout merges them over its built-in defaults (colour, scale and
  `text-align` inherit down the tree).
* `slweb_color_parse` understands named colours, `#rgb`/`#rrggbb` and `rgb(r,g,b)`.
* Layout emits an `SLWEB_BOX_RECT` behind every box with a background, and text boxes carry their
  `fg`/`bg`/`underline`; images become `SLWEB_BOX_IMAGE` boxes pointing at a decoded bitmap.

Images are decoded with `slweb_image_decode`, which sniffs the format:

* **PNG** — baseline, non-interlaced: 8-bit grey / RGB / palette / grey+alpha / RGBA, plus 1/2/4-bit
  grey and palette. `png.c` carries its own `inflate`, so there is no zlib dependency.
* **BS64** — the StarlightOS raw "basesize64" bitmap:
  `BS64` magic, byte 4 version (1), byte 5 flags (bit 0 set = 4 channels, else 3), bytes 6–7 reserved,
  bytes 8–9 width (u16 LE), bytes 10–11 height (u16 LE), then packed RGB or RGBA rows.
* **`data:` URIs** — `data:image/png;base64,...` is base64-decoded (`slweb_base64_decode`) and then
  run through the format sniffer, so a page can embed an image with no extra request.

Pixels are stored as `uint32_t` `0xAARRGGBB` (row-major); the browser scales them to the box with
nearest-neighbour sampling and alpha-blends (`0` = transparent).

### HTTP cache (M7)

`cache.c` is an in-memory, LRU HTTP response cache keyed by absolute URL; the browser uses it so
back/forward (and a repeat visit) are served without a second request, while reload bypasses it:

```c
struct slweb_cache *c = slweb_cache_new(64, 8 * 1024 * 1024);   /* entry count, byte budget */
slweb_cache_put(c, url, status, content_type, body, body_len);
const struct slweb_cache_entry *e = slweb_cache_get(c, url);    /* marks it MRU */
if (e) render(e->status, e->content_type, e->body, e->body_len);
slweb_cache_free(c);
```

Entries evict least-recently-used first, on either budget; a body that can never fit returns `-1`.
`slweb_cache_hits`/`_misses` expose counters for logging and tests.

### Forms (M7)

`form.c` collects a form's successful controls, encodes them, and resolves the submission:

```c
struct slweb_form_pair pairs[32];
size_t n = slweb_form_collect(form, my_value_fn, NULL, pairs, 32);   /* form==NULL: whole doc */
char query[1024];
slweb_form_query(pairs, n, query, sizeof query);                     /* "q=a+b&c=on"           */

char url[512], body[1024]; int is_post;
slweb_form_submit(page_url, form, query, url, sizeof url, &is_post, body, sizeof body);
/* GET: query merged into the action URL's query; POST: query is the body. */
```

* `slweb_form_collect` walks the subtree in document order and keeps named, successful controls:
  text inputs, `<textarea>`, `<select>` (the selected `<option>`'s value/text), checked
  checkbox/radio (`on` by default), and the *activated* submit button. `my_value_fn` supplies each
  control's **live** value (the browser keeps edited values keyed by node).
* `slweb_url_encode` is `application/x-www-form-urlencoded`: space -> `+`, other non-`[A-Za-z0-9-_.~]`
  -> `%XX`.
* Layout emits widget boxes: `<input>`/`<textarea>`/`<select>` become `SLWEB_BOX_INPUT` and
  `<button>`/submit/reset become `SLWEB_BOX_BUTTON`, each carrying a `node` back-pointer; `hidden`
  inputs and `option`/`optgroup` emit no box.

## Tests

```sh
make slwebtest
```

Compiles `tests/host/slweb_host.c` (a scripted, in-memory replacement for the socket syscalls)
against `url.c`/`http.c`/`html.c`/`dom.c`/`layout.c`/`css.c`/`image.c`/`png.c`/`cache.c`/`form.c` and
runs `tests/host/slweb_test.py`: URL, HTTP, the DOM tree, exact box geometry (`dom`/`layout WIDTH`
modes), the CSS cascade (`css` mode), PNG/BS64/`data:` decoding (`image` mode), the cache
(`cache` mode) and form encoding (`form` mode). It then re-runs the kernel HTML corpus
(`tests/host/html_test.py`) against the lifted renderer. `python3 tests/run_tests.py web` drives
`apps/WEBTEST.ELF` in QEMU against the HTTP fixtures, and `... browse` drives `apps/BROWSER.ELF`
(link following, back/reload, the M7 address bar + back/forward cache + bookmarks + a form
submission, and a styled page with an inline `data:` image and a fetched PNG).
