/* http.h - tiny single-threaded HTTP/1.1 server (Connection: close). */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "buf.h"

typedef struct {
    char   method[8];
    char  *path;     /* URL-decoded, without query string */
    char  *query;    /* raw query string ("" if none) */
    char  *body;     /* NUL-terminated */
    size_t body_len;
} HttpReq;

typedef struct {
    int         status;
    const char *ctype;
    Buf         body;
} HttpRes;

typedef void (*HttpHandler)(HttpReq *req, HttpRes *res, void *ud);

/* Blocks forever serving requests. Returns -1 if the socket can't be set up. */
int http_serve(const char *host, int port, HttpHandler h, void *ud);

const char *http_status_text(int status);

#endif
