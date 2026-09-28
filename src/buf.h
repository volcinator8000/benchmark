/* buf.h - growable byte buffer used for building strings and responses. */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>

typedef struct {
    char  *data; /* always NUL-terminated when non-NULL */
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_reset(Buf *b);
void  buf_append(Buf *b, const char *s, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_putc(Buf *b, char c);
void  buf_printf(Buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void  buf_html_escape(Buf *b, const char *s, size_t n);
char *buf_steal(Buf *b); /* returns heap string ("" if empty), resets b */

/* strdup / strndup that abort on OOM, so callers stay simple */
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);

#endif
