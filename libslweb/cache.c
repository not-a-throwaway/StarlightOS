/*
 * cache.c -- M7: a small in-memory HTTP GET cache for libslweb.
 *
 * The browser re-fetches a URL whenever it navigates. With a cache, going
 * back/forward (or revisiting a link) is instant and works even if the origin
 * has gone away. Entries are keyed by absolute URL and evicted least-recently-
 * used once either the entry count or the total byte budget is exceeded.
 *
 * Only the pieces a browser needs to re-render a page are kept: the HTTP
 * status, the Content-Type and the decoded body. Callers copy what they need
 * before the entry can be evicted (any put() may drop the oldest entry).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

struct entry {
    struct slweb_cache_entry pub;   /* url / status / content_type / body / len */
    uint64_t used;                  /* LRU stamp                                */
};

struct slweb_cache {
    struct entry *e;
    size_t n;                       /* entries in use                           */
    size_t cap;                     /* maximum entries                          */
    size_t max_bytes;               /* body budget                              */
    size_t bytes;                   /* bodies currently held                    */
    uint64_t tick;
    int hits, misses;
};

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void entry_free(struct entry *e)
{
    free((void *)e->pub.url);
    free((void *)e->pub.body);
    memset(e, 0, sizeof *e);
}

/* Drop entry i, compacting the array. */
static void entry_remove(struct slweb_cache *c, size_t i)
{
    c->bytes -= c->e[i].pub.body_len;
    entry_free(&c->e[i]);
    if (i + 1 < c->n) memmove(&c->e[i], &c->e[i + 1], (c->n - i - 1) * sizeof c->e[0]);
    c->n--;
}

static void evict_lru(struct slweb_cache *c)
{
    if (c->n == 0) return;
    size_t victim = 0;
    for (size_t i = 1; i < c->n; i++)
        if (c->e[i].used < c->e[victim].used) victim = i;
    entry_remove(c, victim);
}

struct slweb_cache *slweb_cache_new(size_t max_entries, size_t max_bytes)
{
    if (max_entries < 1) max_entries = 1;
    struct slweb_cache *c = malloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    c->cap = max_entries;
    c->max_bytes = max_bytes;
    c->e = malloc(max_entries * sizeof c->e[0]);
    if (!c->e) { free(c); return NULL; }
    memset(c->e, 0, max_entries * sizeof c->e[0]);
    return c;
}

void slweb_cache_clear(struct slweb_cache *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->n; i++) entry_free(&c->e[i]);
    c->n = 0;
    c->bytes = 0;
}

void slweb_cache_free(struct slweb_cache *c)
{
    if (!c) return;
    slweb_cache_clear(c);
    free(c->e);
    free(c);
}

int slweb_cache_count(const struct slweb_cache *c) { return c ? (int)c->n : 0; }
size_t slweb_cache_bytes(const struct slweb_cache *c) { return c ? c->bytes : 0; }
int slweb_cache_hits(const struct slweb_cache *c) { return c ? c->hits : 0; }
int slweb_cache_misses(const struct slweb_cache *c) { return c ? c->misses : 0; }

const struct slweb_cache_entry *slweb_cache_get(struct slweb_cache *c, const char *url)
{
    if (!c || !url) return NULL;
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].pub.url, url) == 0) {
            c->e[i].used = ++c->tick;
            c->hits++;
            return &c->e[i].pub;
        }
    }
    c->misses++;
    return NULL;
}

int slweb_cache_put(struct slweb_cache *c, const char *url, int status,
                    const char *content_type, const uint8_t *body, size_t body_len)
{
    if (!c || !url || !url[0]) return -1;
    if (body_len > c->max_bytes) return -1;   /* can never fit */

    /* replace an existing entry in place */
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].pub.url, url) == 0) {
            uint8_t *copy = NULL;
            if (body_len || body) {
                copy = malloc(body_len + 1);
                if (!copy) return -1;
                if (body_len) memcpy(copy, body, body_len);
                copy[body_len] = 0;
            }
            c->bytes -= c->e[i].pub.body_len;
            free((void *)c->e[i].pub.body);
            c->e[i].pub.body = copy;
            c->e[i].pub.body_len = body_len;
            c->e[i].pub.status = status;
            c->bytes += body_len;
            snprintf(c->e[i].pub.content_type, sizeof c->e[i].pub.content_type, "%s",
                     content_type ? content_type : "");
            c->e[i].used = ++c->tick;
            return 0;
        }
    }

    /* make room: entry budget, then byte budget */
    if (c->n >= c->cap) evict_lru(c);
    while (c->n > 0 && c->bytes + body_len > c->max_bytes) evict_lru(c);

    char *u = dup_str(url);
    if (!u) return -1;
    uint8_t *copy = NULL;
    if (body_len || body) {
        copy = malloc(body_len + 1);
        if (!copy) { free(u); return -1; }
        if (body_len) memcpy(copy, body, body_len);
        copy[body_len] = 0;
    }
    struct entry *e = &c->e[c->n++];
    memset(e, 0, sizeof *e);
    e->pub.url = u;
    e->pub.status = status;
    e->pub.body = copy;
    e->pub.body_len = body_len;
    snprintf(e->pub.content_type, sizeof e->pub.content_type, "%s", content_type ? content_type : "");
    e->used = ++c->tick;
    c->bytes += body_len;
    return 0;
}
