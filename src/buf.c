#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void) {
    fputs("out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

void buf_init(Buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    buf_init(b);
}

void buf_reset(Buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    b->data = xrealloc(b->data, ncap);
    b->cap = ncap;
}

void buf_append(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_putc(Buf *b, char c) { buf_append(b, &c, 1); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void buf_html_escape(Buf *b, const char *s, size_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default: break;
        }
        if (rep) {
            buf_append(b, s + start, i - start);
            buf_puts(b, rep);
            start = i + 1;
        }
    }
    buf_append(b, s + start, n - start);
}

char *buf_steal(Buf *b) {
    char *d = b->data ? b->data : xstrdup("");
    buf_init(b);
    return d;
}
