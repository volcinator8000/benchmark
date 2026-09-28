#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Growable string buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_append(StrBuf *sb, const char *s);
void sb_appendc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_append_html_escaped(StrBuf *sb, const char *s);
/* Returns the NUL-terminated contents (caller frees) and resets the buffer. */
char *sb_detach(StrBuf *sb);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Reads a whole file. Returns NULL if it cannot be opened. */
char *read_file(const char *path, size_t *len_out);
/* Writes to path.tmp then renames over path. Returns 0 on success. */
int write_file_atomic(const char *path, const char *data, size_t len);
int ensure_dir(const char *path);

int str_starts_with(const char *s, const char *prefix);
int str_ends_with(const char *s, const char *suffix);

#endif
