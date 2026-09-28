#define _POSIX_C_SOURCE 200809L
#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADER (16 * 1024)
#define MAX_BODY   (1024 * 1024)

const char *http_status_text(int s) {
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *s, size_t n) {
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            char c = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            out[j++] = c ? c : '?'; /* never embed NUL */
            i += 2;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_simple(int fd, int status, const char *msg) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, http_status_text(status), strlen(msg));
    write_all(fd, hdr, (size_t)n);
    write_all(fd, msg, strlen(msg));
}

static void handle_conn(int fd, HttpHandler h, void *ud) {
    Buf in;
    buf_init(&in);
    char chunk[4096];
    char *hdr_end = NULL;

    while (!hdr_end) {
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) goto done;
        buf_append(&in, chunk, (size_t)r);
        hdr_end = strstr(in.data, "\r\n\r\n");
        if (!hdr_end && in.len > MAX_HEADER) { send_simple(fd, 413, "headers too large"); goto done; }
    }

    size_t hdr_len = (size_t)(hdr_end - in.data) + 4;
    long content_length = 0;

    /* Request line */
    HttpReq req;
    memset(&req, 0, sizeof req);
    char *line_end = strstr(in.data, "\r\n");
    char *sp1 = memchr(in.data, ' ', (size_t)(line_end - in.data));
    char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1)) : NULL;
    if (!sp1 || !sp2 || (size_t)(sp1 - in.data) >= sizeof req.method) {
        send_simple(fd, 400, "bad request line");
        goto done;
    }
    memcpy(req.method, in.data, (size_t)(sp1 - in.data));

    /* Headers: we only care about Content-Length */
    for (char *l = line_end + 2; l < hdr_end; ) {
        char *e = strstr(l, "\r\n");
        if (!e) break;
        if ((size_t)(e - l) > 15 && strncasecmp(l, "Content-Length:", 15) == 0) {
            char *endp;
            content_length = strtol(l + 15, &endp, 10);
            if (content_length < 0) content_length = -1;
        }
        l = e + 2;
    }
    if (content_length < 0) { send_simple(fd, 400, "bad content-length"); goto done; }
    if (content_length > MAX_BODY) { send_simple(fd, 413, "body too large"); goto done; }

    while (in.len - hdr_len < (size_t)content_length) {
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) goto done;
        buf_append(&in, chunk, (size_t)r);
    }

    /* Target: path + query */
    char *target = sp1 + 1;
    size_t tlen = (size_t)(sp2 - target);
    char *q = memchr(target, '?', tlen);
    size_t plen = q ? (size_t)(q - target) : tlen;
    req.path = url_decode(target, plen);
    req.query = q ? xstrndup(q + 1, tlen - plen - 1) : xstrdup("");
    req.body = xstrndup(in.data + hdr_len, (size_t)content_length);
    req.body_len = (size_t)content_length;

    HttpRes res;
    res.status = 200;
    res.ctype = "text/html; charset=utf-8";
    buf_init(&res.body);

    h(&req, &res, ud);

    Buf out;
    buf_init(&out);
    buf_printf(&out,
               "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
               "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
               "Connection: close\r\n\r\n",
               res.status, http_status_text(res.status), res.ctype, res.body.len);
    if (strcmp(req.method, "HEAD") != 0 && res.body.len)
        buf_append(&out, res.body.data, res.body.len);
    write_all(fd, out.data, out.len);

    fprintf(stderr, "%s %s -> %d\n", req.method, req.path, res.status);

    buf_free(&out);
    buf_free(&res.body);
    free(req.path);
    free(req.query);
    free(req.body);
done:
    buf_free(&in);
}

int http_serve(const char *host, int port, HttpHandler h, void *ud) {
    signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad host address: %s\n", host);
        close(s);
        return -1;
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(s); return -1; }
    if (listen(s, 64) < 0) { perror("listen"); close(s); return -1; }

    fprintf(stderr, "listening on http://%s:%d\n", host, port);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        struct timeval tv = { 5, 0 }; /* don't let a slow client stall us */
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle_conn(c, h, ud);
        close(c);
    }
}
