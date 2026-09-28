#ifndef HTTP_H
#define HTTP_H

#include "util.h"
#include <stddef.h>

typedef struct {
    char method[16];
    char path[2048];      /* percent-decoded, without query string */
    char query[2048];
    char content_type[128];
    char *body;           /* NUL-terminated as a convenience */
    size_t body_len;
} HttpRequest;

typedef struct {
    int status;
    char content_type[128];
    StrBuf body;
} HttpResponse;

typedef void (*HttpHandler)(const HttpRequest *req, HttpResponse *res, void *userdata);

/* Convenience setters. */
void http_reply(HttpResponse *res, int status, const char *content_type, const char *body, size_t len);
void http_reply_text(HttpResponse *res, int status, const char *text);
const char *http_status_text(int status);
const char *http_mime_for_path(const char *path);

/* Blocking accept loop; returns only on fatal error (-1). */
int http_serve(const char *host, int port, HttpHandler handler, void *userdata);

#endif
