/*
 * libslweb -- URL parsing.
 *
 * Lifted from the kernel's url.c (M3) and adapted to newlib: only the includes
 * and the public name changed. Defaults a schemeless "host/path" to http/80,
 * trims "#fragment", and rejects IPv6 literals.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "slweb.h"

int slweb_url_parse(const char *url, struct slweb_url *o)
{
    memset(o, 0, sizeof(*o));
    if (strncasecmp(url, "http://", 7) == 0) { url += 7; o->port = 80; }
    else if (strncasecmp(url, "https://", 8) == 0) { url += 8; o->port = 443; o->https = 1; }
    else o->port = 80;                         /* bare "example.com/x" */
    unsigned h = 0;
    if (*url == '[') return -1;                /* IPv6 literal unsupported */
    while (*url && *url != '/' && *url != ':' && *url != '?' && *url != '#' && h < sizeof(o->host) - 1) o->host[h++] = *url++;
    o->host[h] = 0;
    if (!h) return -1;
    if (*url == ':') {
        url++;
        unsigned port = 0, d = 0;
        while (*url >= '0' && *url <= '9') { port = port * 10 + (unsigned)(*url++ - '0'); if (++d > 5 || port > 65535) return -1; }
        if (!d || !port) return -1;
        o->port = (uint16_t)port;
    }
    unsigned p = 0;
    if (*url != '/') o->path[p++] = '/';
    if (*url == '?') { /* "host?query" -> "/?query" already has '/' */ }
    while (*url && *url != '#' && p < sizeof(o->path) - 1) o->path[p++] = *url++;
    o->path[p] = 0;
    return 0;
}

int slweb_url_resolve(const char *base, const char *loc, char *out, size_t cap)
{
    if (!loc) loc = "";

    /* Already absolute (either scheme). */
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0) {
        size_t n = strlen(loc);
        if (n + 1 > cap) return -1;
        memcpy(out, loc, n + 1);
        return 0;
    }

    struct slweb_url b;
    if (slweb_url_parse(base, &b) != 0) return -1;

    char hostport[120];
    if (b.port == (b.https ? 443 : 80)) snprintf(hostport, sizeof(hostport), "%s", b.host);
    else snprintf(hostport, sizeof(hostport), "%s:%u", b.host, b.port);
    const char *scheme = b.https ? "https" : "http";

    int n;
    if (loc[0] == '/' && loc[1] == '/') {
        n = snprintf(out, cap, "%s:%s", scheme, loc);
    } else if (loc[0] == '/') {
        n = snprintf(out, cap, "%s://%s%s", scheme, hostport, loc);
    } else {
        /* relative to the base path's directory */
        char dir[200];
        if (strlen(b.path) >= sizeof(dir)) return -1;
        strcpy(dir, b.path);
        char *q = strchr(dir, '?'); if (q) *q = 0;
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else strcpy(dir, "/");
        n = snprintf(out, cap, "%s://%s%s%s", scheme, hostport, dir, loc);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}
