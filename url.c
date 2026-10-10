#include "net.h"
#include "kstring.h"

int url_parse(const char *url, struct url_parts *o)
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
