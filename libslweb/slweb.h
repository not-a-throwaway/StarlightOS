#ifndef STARLIGHTOS_SLWEB_H
#define STARLIGHTOS_SLWEB_H

/*
 * libslweb -- the userland half of the StarlightOS browser.
 *
 * M3 lifts the HTTP client, URL parser and HTML->text renderer out of the
 * kernel (url.c / http.c / html.c) and into a small library a ring-3 program
 * links against. It sits on top of:
 *
 *   - newlib        : malloc, stdio, string/ctype
 *   - user/slnet.h  : blocking IPv4 TCP sockets (sl_socket/sl_connect/...)
 *
 * so a browser no longer needs kernel privileges to talk HTTP. M9.4 lifts the
 * kernel's TLS client (tls.c + crypto.c + x509.c, with tls_user.c as the ring-3
 * platform layer) into the library: an https:// URL performs a full handshake and
 * validates the server certificate against a trust store that starts empty, so
 * roots must be loaded with slweb_http_add_ca_pem()/_file() (or a request can opt
 * out with opts.insecure). A certificate that does not verify fails closed.
 *
 * The public names are prefixed slweb_ to stay clear of newlib and of the
 * kernel's own url_parse/http_request/html_to_text. The structs deliberately
 * mirror the kernel ones (net.h) so the two implementations stay close.
 *
 * This header is freestanding: <stddef.h>/<stdint.h> only.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLWEB_VERSION "4.0"

/* ---- error codes returned by slweb_http_request (see slweb_http_error_text) ---- */
#define SLWEB_OK               0
#define SLWEB_ERR_RESOLVE     -1   /* could not resolve host                       */
#define SLWEB_ERR_CONNECT     -2   /* connection failed / refused                  */
#define SLWEB_ERR_SEND        -3   /* send failed                                  */
#define SLWEB_ERR_TIMEOUT     -4   /* no response (timed out)                      */
#define SLWEB_ERR_MALFORMED   -5   /* malformed HTTP response                      */
#define SLWEB_ERR_REDIRECTS   -6   /* too many redirects                           */
#define SLWEB_ERR_TLS         -7   /* TLS handshake / certificate verification failed */
#define SLWEB_ERR_NOMEM       -8   /* out of memory                                */
#define SLWEB_ERR_INTR        -9   /* interrupted (Ctrl-C)                         */
#define SLWEB_ERR_NONET      -10   /* network not configured (no DHCP lease yet)   */
#define SLWEB_ERR_BADURL     -11   /* bad URL                                      */
#define SLWEB_ERR_HTTPS      -12   /* https is unavailable in this build           */

/* ---- URL parsing --------------------------------------------------------- */
/*
 * Splits "http://host:port/path" (or a bare "host/path", defaulting to
 * http/80). https:// sets ->https and port 443. IPv6 literals are rejected
 * (-1). host/path are NUL-terminated and truncated to their array sizes.
 */
struct slweb_url { char host[96]; uint16_t port; char path[200]; int https; };
int slweb_url_parse(const char *url, struct slweb_url *out);   /* 0, or -1 */

/*
 * Resolves a possibly-relative link (as found in an <a href>) against a base
 * URL: absolute http(s) URLs are copied, "//host/x" inherits the scheme,
 * "/x" keeps host:port, and anything else is resolved against the base path's
 * directory. Writes a NUL-terminated URL to out and returns 0, or -1 if base
 * does not parse or the result would not fit.
 */
int slweb_url_resolve(const char *base, const char *loc, char *out, size_t cap);

/* ---- HTML -> text -------------------------------------------------------- */
/* Hrefs collected while rendering, in document order; the [n] markers the
 * renderer emits match links[n-1]. */
#define SLWEB_MAX_LINKS 32
#define SLWEB_LINK_MAX  256
struct slweb_link { char href[SLWEB_LINK_MAX]; };

/*
 * Renders HTML to plain wrapped text (no layout engine: block structure,
 * lists, headings, links, entities, <pre>; <script>/<style>/<head> skipped).
 * Writes a NUL-terminated string to out and returns its length. show_links
 * appends a numbered list of the links collected while rendering.
 */
size_t slweb_html_to_text(const char *html, size_t len, char *out, size_t cap,
                          int width, int show_links);

/*
 * As slweb_html_to_text, but also copies up to max_links hrefs (document
 * order, matching the [n] markers) into links[] and sets *n_links to the
 * count. links may be NULL and n_links may be NULL. Returns the text length.
 */
size_t slweb_html_to_text_ex(const char *html, size_t len, char *out, size_t cap,
                             int width, int show_links,
                             struct slweb_link *links, size_t max_links, size_t *n_links);

/* ---- HTTP ---------------------------------------------------------------- */
struct slweb_http_opts {
    const char *method;            /* NULL = GET                                   */
    const char *body;              /* POST body, may be NULL                       */
    size_t      body_len;
    const char *extra_headers;     /* "Name: value\r\n..."                         */
    int         follow_redirects;  /* max hops, 0 = don't follow                   */
    uint32_t    timeout_ms;        /* per-read timeout, 0 = default (8 s)          */
    size_t      max_body;          /* 0 = default (2 MiB)                          */
    int         verbose;           /* print request/response head lines (debug)    */
    int         quiet;             /* suppress redirect progress output            */
    int         insecure;          /* 1 = skip certificate validation (curl -k)    */
    void      (*progress)(size_t received, int total);   /* optional              */
};
struct slweb_response {
    int      status;               /* 0 if no valid response                       */
    char     status_text[40];
    char     content_type[64];
    char     location[192];
    char     final_url[256];
    char     headers[1536];        /* raw header block of the final response       */
    uint8_t *body;                 /* malloc'd, NUL-terminated; free with the fn   */
    size_t   body_len;
    int      content_length;       /* -1 if unknown                                */
    int      truncated;
    uint32_t elapsed_ms;
    int      redirects;
    int      tls_verified;         /* 1 if a certificate chain verified (https)    */
    char     tls_peer[128];        /* peer certificate CN ("" if none/unknown)     */
    int      tls_error;            /* TLS_E_* when a handshake failed, else 0      */
    int      tls_alert;            /* server alert byte, -1 if none                */
    int      error;                /* 0 ok, else negative code (see slweb_http_error_text) */
};
int  slweb_http_request(const char *url, const struct slweb_http_opts *opts,
                        struct slweb_response *res);
const char *slweb_http_error_text(int code);
const char *slweb_http_tls_error_text(int tls_error);
void slweb_http_response_free(struct slweb_response *res);

/* ---- TLS trust store (M9.4) ----
 * Roots are PEM certificates; they persist for the life of the process and are
 * shared with the kernel TLS client's global store. add_ca_pem() returns the
 * number of certificates added (>= 0) or -1; add_ca_file() reads a PEM bundle
 * from the filesystem. The store starts empty, so https:// fails closed until
 * roots are loaded (or a request uses opts.insecure). */
int  slweb_http_add_ca_pem(const char *pem, size_t len);
int  slweb_http_add_ca_file(const char *path);
void slweb_http_reset_ca(void);

/* ================================================================== M5: DOM + box layout
 *
 * M4 rendered a page to plain text and the browser kept it as an array of
 * lines. M5 replaces that with a real (small) DOM tree and a box-layout pass:
 *
 *   dom.c     parses HTML into slweb_node elements/text, decoded entities and
 *             attributes, skipping the contents of <script>/<style>/<title>,
 *             handling void elements and the usual implicit closes.
 *   layout.c  walks the tree and produces a flat, ordered list of slweb_box
 *             rectangles (wrapped text runs, list markers, rules and image
 *             placeholders) in a viewport of a given width. Links are handed
 *             back in document order, matching struct slweb_link (as in M4).
 *
 * The layout engine has no colours and no font of its own: metrics are fixed
 * (an 8x8 glyph in a 10 px line, doubled for <h1>/<h2>), so the renderer can be
 * the browser's bitmap font and the host tests can assert on exact geometry.
 */

enum {
    SLWEB_NODE_DOCUMENT = 0,
    SLWEB_NODE_ELEMENT  = 1,
    SLWEB_NODE_TEXT     = 2
};

#define SLWEB_TAG_MAX   32
#define SLWEB_ATTR_MAX  12
#define SLWEB_ATTR_NAME 28
#define SLWEB_ATTR_VAL  128

struct slweb_attr {
    char name[SLWEB_ATTR_NAME];
    char value[SLWEB_ATTR_VAL];
};

struct slweb_node {
    int type;                          /* SLWEB_NODE_*                        */
    char tag[SLWEB_TAG_MAX];           /* element: lowercased tag; doc: ""    */
    struct slweb_attr attrs[SLWEB_ATTR_MAX];
    int nattrs;
    char *text;                        /* text node: malloc'd, NUL-terminated */
    size_t text_len;
    struct slweb_node *parent;
    struct slweb_node *first_child;
    struct slweb_node *last_child;
    struct slweb_node *next_sibling;
};

/* Parse an HTML body into a document node (never NULL unless out of memory). */
struct slweb_node *slweb_dom_parse(const char *html, size_t len);
/* A document whose body is one <pre> holding the bytes verbatim (plain text). */
struct slweb_node *slweb_dom_from_text(const char *text, size_t len);
void slweb_dom_free(struct slweb_node *node);
/* First attribute value or NULL; attribute names are compared case-insensitively. */
const char *slweb_node_attr(const struct slweb_node *node, const char *name);

enum {
    SLWEB_BOX_TEXT   = 0,   /* a wrapped run of text on one line              */
    SLWEB_BOX_IMAGE  = 1,   /* inline image (bitmap in M6) or alt placeholder */
    SLWEB_BOX_RULE   = 2,   /* horizontal rule (<hr>)                          */
    SLWEB_BOX_MARKER = 3,   /* list marker: marker = 1-based number, 0 bullet  */
    SLWEB_BOX_RECT   = 4,   /* M6: a filled background/border rectangle        */
    SLWEB_BOX_INPUT  = 5,   /* M7: <input>/<textarea>/<select> widget          */
    SLWEB_BOX_BUTTON = 6    /* M7: <button> / <input type=submit|reset|button> */
};

/* Fixed metrics shared by the layout engine and the renderer. */
#define SLWEB_GLYPH_W   8
#define SLWEB_GLYPH_H   8
#define SLWEB_LINE_H    10      /* scale 1 line box height                     */
#define SLWEB_LINE_H2   16      /* scale 2 (h1/h2) line box height             */

struct slweb_image;    /* defined below (M6) */

struct slweb_box {
    uint8_t kind;           /* SLWEB_BOX_*                                 */
    uint8_t scale;          /* 1 = 8x8 glyphs, 2 = 16x16 (h1/h2)           */
    uint8_t bold;
    uint8_t italic;
    int link;               /* 1-based link index into links[], 0 = none   */
    int marker;             /* SLWEB_BOX_MARKER: number, 0 = bullet        */
    int x, y, w, h;         /* layout-space rectangle (pixels, y down)     */
    const char *text;       /* text_len bytes; points into the DOM         */
    int text_len;
    /* ---- M6 ---- */
    uint8_t underline;      /* text-decoration: underline                  */
    uint8_t fg_set;         /* 1 = this box has an author colour           */
    uint8_t bg_set;         /* 1 = this box has an author background       */
    uint8_t _pad;
    uint32_t fg;            /* 0x00RRGGBB (valid when fg_set)              */
    uint32_t bg;            /* 0x00RRGGBB (valid when bg_set)              */
    const struct slweb_image *image;   /* SLWEB_BOX_IMAGE: decoded bitmap, or NULL */
    const struct slweb_node *node;     /* M7: source element for INPUT/BUTTON boxes */
};

#define SLWEB_MAX_BOXES 16384

/* Lay the document out in a viewport of viewport_w CSS pixels. Fills boxes[0..n)
 * and returns the number of boxes emitted (capped at max_boxes). links[], if
 * given, receives the anchor hrefs in document order (matching box.link); the
 * count is stored in *n_links. *doc_w and *doc_h receive the laid-out size. */
int slweb_layout(const struct slweb_node *root, int viewport_w,
                 struct slweb_box *boxes, int max_boxes,
                 struct slweb_link *links, size_t max_links, size_t *n_links,
                 int *doc_w, int *doc_h);

/* ============================================================================
 * M6: CSS subset + bitmap images
 * ========================================================================= */

/* ---- colours ------------------------------------------------------------- */
/* 0x00RRGGBB. */
int slweb_color_parse(const char *s, uint32_t *out);   /* 0 ok, -1 unknown */

/* ---- bitmap images ------------------------------------------------------- */
/*
 * Decoded bitmap: w*h pixels, row-major, each 0xAARRGGBB (alpha 0xFF when the
 * source is opaque). Formats understood: PNG (baseline, non-interlaced: 8-bit
 * grey / RGB / palette / grey+alpha / RGBA, plus 1/2/4-bit grey and palette) and
 * the StarlightOS "basesize64" raw bitmap (magic "BS64", see libslweb/README).
 */
struct slweb_image {
    int w, h;
    int has_alpha;          /* 1 if any pixel is not fully opaque          */
    uint32_t *pixels;       /* malloc'd w*h, 0xAARRGGBB                    */
};

/* Sniff a decoded-on-disk format: "png", "bs64", or NULL. */
const char *slweb_image_format(const uint8_t *data, size_t len);
/* Decode an image from memory. Returns 0 and fills *out, or -1 on error. */
int slweb_image_decode(const uint8_t *data, size_t len, struct slweb_image *out);
void slweb_image_free(struct slweb_image *img);

/* base64 (RFC 4648, '=' padding, whitespace ignored): decoded length or -1. */
long slweb_base64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap);

/* Decode a data: URI ("data:image/png;base64,...."). 0 ok, -1 otherwise. */
int slweb_image_data_uri(const char *uri, struct slweb_image *out);

/* Low-level decoders (usually reached through slweb_image_decode). */
int slweb_png_decode(const uint8_t *data, size_t len, struct slweb_image *out);
int slweb_bs64_decode(const uint8_t *data, size_t len, struct slweb_image *out);

/* ---- CSS ----------------------------------------------------------------- */
enum { SLWEB_ALIGN_LEFT = 0, SLWEB_ALIGN_CENTER = 1, SLWEB_ALIGN_RIGHT = 2 };

/*
 * A *declaration set*: each property has a "_set" flag. slweb_style_compute
 * fills only the properties the author actually declared (from <style> rules
 * and the element's style="" attribute), so the layout engine can merge them
 * over its built-in defaults and inherit colour/scale/align down the tree.
 */
struct slweb_style {
    int      color_set;    uint32_t color;
    int      bg_set;       uint32_t bg;
    int      bold_set;     int bold;         /* font-weight                 */
    int      italic_set;   int italic;       /* font-style                  */
    int      underline_set;int underline;    /* text-decoration             */
    int      scale_set;    int scale;        /* font-size -> 1 or 2         */
    int      display_set;  int display_none; /* display: none               */
    int      align_set;    int align;        /* SLWEB_ALIGN_*               */
    int      margin_set[4];   int margin[4];    /* top,right,bottom,left (px)  */
    int      pad_set[4];      int pad[4];       /* top,right,bottom,left (px)  */
    int      width_set;    int width;        /* px                          */
    int      height_set;   int height;       /* px                          */
};

struct slweb_css;
/* Walk a DOM, collecting <style> contents into a stylesheet (never NULL). */
struct slweb_css *slweb_css_parse(const struct slweb_node *root);
void slweb_css_free(struct slweb_css *css);
/* Number of rules parsed (for logging). */
int slweb_css_rule_count(const struct slweb_css *css);
/* Author declarations applying to `node` (stylesheet + inline style). */
void slweb_style_compute(const struct slweb_css *css, const struct slweb_node *node,
                         struct slweb_style *out);

/* ---- layout with CSS + images ------------------------------------------- */
struct slweb_image_entry {
    const struct slweb_node *node;
    const struct slweb_image *image;
};
struct slweb_layout_opts {
    const struct slweb_css *css;               /* may be NULL                 */
    const struct slweb_image_entry *images;    /* may be NULL                 */
    size_t n_images;
};

/*
 * As slweb_layout, but applies a stylesheet and paints decoded <img> bitmaps.
 * opts may be NULL (identical to slweb_layout). Boxes get their fg/bg/underline
 * and (for images) a decoded bitmap pointer; background rectangles are emitted
 * as SLWEB_BOX_RECT before their content.
 */
int slweb_layout_ex(const struct slweb_node *root, int viewport_w,
                    const struct slweb_layout_opts *opts,
                    struct slweb_box *boxes, int max_boxes,
                    struct slweb_link *links, size_t max_links, size_t *n_links,
                    int *doc_w, int *doc_h);

/* ============================================================================
 * M7: HTTP cache + forms
 * ========================================================================= */

/* ---- HTTP cache ---------------------------------------------------------- */
/*
 * An in-memory, size-bounded GET cache keyed by absolute URL. Bodies are
 * copied in and out live until the entry is evicted or the cache is freed.
 * Eviction is least-recently-used once either the entry count or the byte
 * budget is exceeded. The browser uses it so back/forward navigation (and a
 * repeated visit) can be served without a second request; reload bypasses it.
 */
struct slweb_cache;
struct slweb_cache_entry {
    const char *url;
    int      status;
    char     content_type[64];
    const uint8_t *body;
    size_t   body_len;
};

struct slweb_cache *slweb_cache_new(size_t max_entries, size_t max_bytes);
void slweb_cache_free(struct slweb_cache *c);
void slweb_cache_clear(struct slweb_cache *c);
/* Look up an entry (marks it most-recently-used). Returns it, or NULL. */
const struct slweb_cache_entry *slweb_cache_get(struct slweb_cache *c, const char *url);
/* Store/replace an entry. Returns 0, or -1 if the body would never fit. */
int  slweb_cache_put(struct slweb_cache *c, const char *url, int status,
                     const char *content_type, const uint8_t *body, size_t body_len);
int  slweb_cache_count(const struct slweb_cache *c);
size_t slweb_cache_bytes(const struct slweb_cache *c);
/* hits/misses counters (for logging/tests). */
int  slweb_cache_hits(const struct slweb_cache *c);
int  slweb_cache_misses(const struct slweb_cache *c);

/* ---- forms --------------------------------------------------------------- */
/* application/x-www-form-urlencoded: space -> '+', other non-alphanumerics
 * (and non - _ . ~) -> %XX. NUL-terminates. Returns the encoded length, or -1
 * if it would not fit in cap. */
int slweb_url_encode(const char *s, char *out, size_t cap);

#define SLWEB_FORM_NAME_MAX  64
#define SLWEB_FORM_VALUE_MAX 256
struct slweb_form_pair {
    char name[SLWEB_FORM_NAME_MAX];
    char value[SLWEB_FORM_VALUE_MAX];
};

/* Join pairs as "a=1&b=2" with each part URL-encoded. NUL-terminates and
 * returns the length, or -1 if it would not fit in cap. */
int slweb_form_query(const struct slweb_form_pair *pairs, size_t n,
                     char *out, size_t cap);

/*
 * A callback that supplies the live value of a control during collection.
 * Return 0 and NUL-terminate `out` to include the control, or -1 to skip it
 * (e.g. an unchecked box, a disabled control, or a submit button that was not
 * the one activated). `user` is passed through.
 */
typedef int (*slweb_form_value_fn)(const struct slweb_node *control, void *user,
                                   char *out, size_t cap);

/*
 * Walk the subtree of `form` (or the whole document when form is NULL) and
 * collect the named, successful controls into out[0..max). `value` fetches
 * each control's current value. Returns the number of pairs written.
 */
size_t slweb_form_collect(const struct slweb_node *form,
                          slweb_form_value_fn value, void *user,
                          struct slweb_form_pair *out, size_t max);

/*
 * Resolve a submission of `form` (found on `page_url`) carrying the already
 * encoded `query`. For method=post, *is_post is set to 1, url_out is the
 * action URL, and body_out holds `query` (the caller sends it with
 * Content-Type: application/x-www-form-urlencoded). For method=get (the
 * default), *is_post is 0 and the query is merged into the action URL's query
 * string. Returns 0, or -1 on a bad action URL.
 */
int slweb_form_submit(const char *page_url, const struct slweb_node *form,
                      const char *query,
                      char *url_out, size_t url_cap,
                      int *is_post, char *body_out, size_t body_cap);

#ifdef __cplusplus
}
#endif

#endif
