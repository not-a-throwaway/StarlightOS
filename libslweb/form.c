/*
 * form.c -- M7: HTML form collection + application/x-www-form-urlencoded.
 *
 * The browser needs three things from a form: the (name, value) pairs of its
 * successful controls, the encoded query string, and the resulting request
 * (GET merges the query into the action URL; POST sends it as the body). The
 * live values (what was typed, which box is checked, the selected option) are
 * owned by the caller, so collection takes a value callback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slweb.h"

static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

int slweb_url_encode(const char *s, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == ' ') {
            if (o + 2 > cap) return -1;
            out[o++] = '+';
        } else if (unreserved(c)) {
            if (o + 2 > cap) return -1;
            out[o++] = (char)c;
        } else {
            if (o + 4 > cap) return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (o + 1 > cap) return -1;
    out[o] = 0;
    return (int)o;
}

int slweb_form_query(const struct slweb_form_pair *pairs, size_t n,
                     char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (i) {
            if (o + 2 > cap) return -1;
            out[o++] = '&';
        }
        int r = slweb_url_encode(pairs[i].name, out + o, cap - o);
        if (r < 0) return -1;
        o += (size_t)r;
        if (o + 2 > cap) return -1;
        out[o++] = '=';
        r = slweb_url_encode(pairs[i].value, out + o, cap - o);
        if (r < 0) return -1;
        o += (size_t)r;
    }
    if (o + 1 > cap) return -1;
    out[o] = 0;
    return (int)o;
}

static int is_control(const char *tag)
{
    return strcmp(tag, "input") == 0 || strcmp(tag, "textarea") == 0 ||
           strcmp(tag, "select") == 0 || strcmp(tag, "button") == 0;
}

static void collect_walk(const struct slweb_node *n,
                         slweb_form_value_fn value, void *user,
                         struct slweb_form_pair *out, size_t max, size_t *count)
{
    for (; n; n = n->next_sibling) {
        if (n->type != SLWEB_NODE_ELEMENT) continue;
        if (is_control(n->tag)) {
            const char *name = slweb_node_attr(n, "name");
            if (!name || !name[0]) continue;
            if (slweb_node_attr(n, "disabled")) continue;
            if (*count >= max) continue;
            char v[SLWEB_FORM_VALUE_MAX];
            v[0] = 0;
            if (value && value(n, user, v, sizeof v) != 0) continue;
            v[sizeof v - 1] = 0;
            snprintf(out[*count].name, sizeof out[*count].name, "%s", name);
            snprintf(out[*count].value, sizeof out[*count].value, "%s", v);
            (*count)++;
            continue;   /* controls are leaves */
        }
        if (n->first_child) collect_walk(n->first_child, value, user, out, max, count);
    }
}

size_t slweb_form_collect(const struct slweb_node *form,
                          slweb_form_value_fn value, void *user,
                          struct slweb_form_pair *out, size_t max)
{
    size_t count = 0;
    if (!form || !out || max == 0) return 0;
    collect_walk(form, value, user, out, max, &count);
    return count;
}

static int eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* Drop any query/fragment so a GET's form data replaces (not appends to) it. */
static void strip_query(char *url)
{
    for (char *p = url; *p; p++)
        if (*p == '?' || *p == '#') { *p = 0; return; }
}

int slweb_form_submit(const char *page_url, const struct slweb_node *form,
                      const char *query,
                      char *url_out, size_t url_cap,
                      int *is_post, char *body_out, size_t body_cap)
{
    const char *action = form ? slweb_node_attr(form, "action") : NULL;
    const char *method = form ? slweb_node_attr(form, "method") : NULL;
    int post = method && eq_ci(method, "post");

    char base[512];
    if (slweb_url_resolve(page_url, (action && action[0]) ? action : page_url,
                          base, sizeof base) != 0)
        return -1;

    /* Reject an action that does not parse (e.g. an IPv6 literal or bad port),
     * even when resolve() simply copied it through as an absolute URL. */
    struct slweb_url check;
    if (slweb_url_parse(base, &check) != 0)
        return -1;

    if (is_post) *is_post = post;
    if (body_out && body_cap) snprintf(body_out, body_cap, "%s", query ? query : "");

    if (post) {
        snprintf(url_out, url_cap, "%s", base);
        return 0;
    }
    strip_query(base);
    if (query && query[0])
        snprintf(url_out, url_cap, "%s?%s", base, query);
    else
        snprintf(url_out, url_cap, "%s", base);
    return 0;
}
