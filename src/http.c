#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_BODY_BYTES   (8 * 1024 * 1024)

void http_reply(HttpResponse *res, int status, const char *content_type, const char *body, size_t len) {
    res->status = status;
    snprintf(res->content_type, sizeof res->content_type, "%s", content_type);
    res->body.len = 0;
    if (res->body.data) res->body.data[0] = '\0';
    sb_appendn(&res->body, body, len);
}

void http_reply_text(HttpResponse *res, int status, const char *text) {
    http_reply(res, status, "text/plain; charset=utf-8", text, strlen(text));
}

const char *http_status_text(int status) {
    switch (status) {
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
    default: return "Unknown";
    }
}

const char *http_mime_for_path(const char *path) {
    if (str_ends_with(path, ".html")) return "text/html; charset=utf-8";
    if (str_ends_with(path, ".css")) return "text/css; charset=utf-8";
    if (str_ends_with(path, ".js")) return "application/javascript; charset=utf-8";
    if (str_ends_with(path, ".json")) return "application/json; charset=utf-8";
    if (str_ends_with(path, ".svg")) return "image/svg+xml";
    if (str_ends_with(path, ".png")) return "image/png";
    if (str_ends_with(path, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dstlen, const char *src, size_t srclen) {
    size_t o = 0;
    for (size_t i = 0; i < srclen && o + 1 < dstlen; i++) {
        if (src[i] == '%' && i + 2 < srclen && hexv(src[i + 1]) >= 0 && hexv(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexv(src[i + 1]) * 16 + hexv(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, const HttpResponse *res) {
    StrBuf head;
    sb_init(&head);
    sb_printf(&head,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "Connection: close\r\n"
              "Cache-Control: no-store\r\n"
              "\r\n",
              res->status, http_status_text(res->status), res->content_type, res->body.len);
    if (send_all(fd, head.data, head.len) == 0) send_all(fd, res->body.data, res->body.len);
    sb_free(&head);
}

static void send_simple(int fd, int status, const char *msg) {
    HttpResponse res;
    memset(&res, 0, sizeof res);
    sb_init(&res.body);
    http_reply_text(&res, status, msg);
    send_response(fd, &res);
    sb_free(&res.body);
}

/* Case-insensitive header lookup within the raw header block. Copies the value. */
static int find_header(const char *headers, const char *name, char *out, size_t outlen) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (v < p + linelen && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = (size_t)(p + linelen - v);
            if (vlen >= outlen) vlen = outlen - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return -1;
}

static void handle_connection(int fd, HttpHandler handler, void *userdata) {
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    StrBuf in;
    sb_init(&in);
    size_t header_end = 0;
    for (;;) {
        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { sb_free(&in); return; }
        sb_appendn(&in, buf, (size_t)n);
        char *he = strstr(in.data, "\r\n\r\n");
        if (he) { header_end = (size_t)(he - in.data) + 4; break; }
        if (in.len > MAX_HEADER_BYTES) { send_simple(fd, 413, "headers too large"); sb_free(&in); return; }
    }
    if (header_end > MAX_HEADER_BYTES) { send_simple(fd, 413, "headers too large"); sb_free(&in); return; }

    HttpRequest req;
    memset(&req, 0, sizeof req);

    /* Request line */
    char *line_end = strstr(in.data, "\r\n");
    *line_end = '\0';
    char *sp1 = strchr(in.data, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) { send_simple(fd, 400, "bad request line"); sb_free(&in); return; }
    snprintf(req.method, sizeof req.method, "%.*s", (int)(sp1 - in.data), in.data);
    const char *target = sp1 + 1;
    size_t target_len = (size_t)(sp2 - target);
    const char *q = memchr(target, '?', target_len);
    if (q) {
        url_decode(req.path, sizeof req.path, target, (size_t)(q - target));
        snprintf(req.query, sizeof req.query, "%.*s", (int)(target + target_len - q - 1), q + 1);
    } else {
        url_decode(req.path, sizeof req.path, target, target_len);
    }

    /* Headers */
    const char *headers = line_end + 2;
    char val[64];
    size_t content_length = 0;
    if (find_header(headers, "Content-Length", val, sizeof val) == 0) {
        long long cl = atoll(val);
        if (cl < 0 || cl > MAX_BODY_BYTES) { send_simple(fd, 413, "body too large"); sb_free(&in); return; }
        content_length = (size_t)cl;
    }
    find_header(headers, "Content-Type", req.content_type, sizeof req.content_type);

    /* Body */
    while (in.len - header_end < content_length) {
        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { sb_free(&in); return; }
        sb_appendn(&in, buf, (size_t)n);
    }
    req.body = in.data + header_end;
    req.body_len = content_length;
    req.body[content_length] = '\0';

    HttpResponse res;
    memset(&res, 0, sizeof res);
    sb_init(&res.body);
    http_reply_text(&res, 404, "not found");
    handler(&req, &res, userdata);
    send_response(fd, &res);
    sb_free(&res.body);
    sb_free(&in);
}

int http_serve(const char *host, int port, HttpHandler handler, void *userdata) {
    signal(SIGPIPE, SIG_IGN);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad host address: %s\n", host);
        close(lfd);
        return -1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(lfd); return -1; }
    if (listen(lfd, 64) < 0) { perror("listen"); close(lfd); return -1; }

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        handle_connection(cfd, handler, userdata);
        close(cfd);
    }
}
