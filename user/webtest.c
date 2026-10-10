/*
 * WEBTEST.ELF -- M3 demo: libslweb (URL + HTTP + HTML->text) on top of the M2
 * network syscalls.
 *
 *   slweb_url_parse()       split "http://host:port/path"
 *   slweb_http_request()    blocking GET with redirects / Content-Length / chunked
 *   slweb_html_to_text()    render an HTML body to wrapped plain text
 *
 * It talks to the test HTTP fixture on 10.0.2.2:18001 (routes /hello, /redir,
 * /loop, /chunked, /page) and prints markers the QEMU test asserts on. The
 * serial log doubles as the test's assertion surface (terminal output is
 * mirrored to COM1).
 */
#include <stdio.h>
#include <string.h>
#include "slweb.h"

#define BASE "http://10.0.2.2:18001"

int main(void)
{
    printf("WEBTEST: start\n");

    /* URL parsing (fragment dropped, query kept, default port). */
    struct slweb_url u;
    if (slweb_url_parse(BASE "/hello?x=1#frag", &u) == 0)
        printf("WEBTEST: url host=%s port=%u https=%d path=%s\n", u.host, (unsigned)u.port, u.https, u.path);

    struct slweb_http_opts o;
    memset(&o, 0, sizeof o);
    o.follow_redirects = 5;
    o.quiet = 1;
    o.timeout_ms = 5000;

    struct slweb_response r;

    /* Plain GET: status line, content-type and body. */
    int err = slweb_http_request(BASE "/hello", &o, &r);
    printf("WEBTEST: http err=%d status=%d type=%s len=%zu\n", err, r.status, r.content_type, r.body_len);
    if (r.body) printf("WEBTEST: body |%s|\n", (const char *)r.body);
    slweb_http_response_free(&r);

    /* 302 -> /hello, followed automatically. */
    err = slweb_http_request(BASE "/redir", &o, &r);
    printf("WEBTEST: redir err=%d status=%d redirects=%d final=%s\n", err, r.status, r.redirects, r.final_url);
    slweb_http_response_free(&r);

    /* A redirect loop must be cut off. */
    err = slweb_http_request(BASE "/loop", &o, &r);
    printf("WEBTEST: loop err=%d (%s)\n", err, slweb_http_error_text(err));
    slweb_http_response_free(&r);

    /* Chunked transfer decoding. */
    err = slweb_http_request(BASE "/chunked", &o, &r);
    printf("WEBTEST: chunked err=%d status=%d len=%zu\n", err, r.status, r.body_len);
    slweb_http_response_free(&r);

    /* HTML page -> text (links numbered, script skipped). */
    err = slweb_http_request(BASE "/page", &o, &r);
    static char text[8192];
    size_t tn = 0;
    if (r.body) tn = slweb_html_to_text((const char *)r.body, r.body_len, text, sizeof text, 60, 1);
    printf("WEBTEST: html err=%d len=%zu |%s|\n", err, tn, text);
    slweb_http_response_free(&r);

    /* https:// is deferred to M9: report it so the browser can show the badge. */
    err = slweb_http_request("https://example.com/", &o, &r);
    printf("WEBTEST: https err=%d (%s)\n", err, slweb_http_error_text(err));
    slweb_http_response_free(&r);

    /* Connection refused and a malformed URL. */
    err = slweb_http_request("http://10.0.2.2:1/", &o, &r);
    printf("WEBTEST: refused err=%d (%s)\n", err, slweb_http_error_text(err));
    slweb_http_response_free(&r);

    err = slweb_http_request("http:///nohost", &o, &r);
    printf("WEBTEST: badurl err=%d\n", err);
    slweb_http_response_free(&r);

    printf("WEBTEST: done\n");
    return 0;
}
