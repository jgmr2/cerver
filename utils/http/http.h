#ifndef HTTP_HELPER_H
#define HTTP_HELPER_H

#include <liburing.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <postgresql/libpq-fe.h>
#include "../events.h"
#include "../json.h" // Importamos el parser JSON minificado

#define INITIAL_READ_BUF_SIZE 4096

typedef enum { OP_WRITE_RES, OP_WRITE_HEADER, OP_READ_FILE, OP_WRITE_FILE, OP_CLOSE, OP_TIMEOUT } req_type_t;

typedef struct {
    event_type_t core_type; req_type_t type; struct io_uring *ring;
    int client_fd, file_fd; struct iovec iov[2];
    char h[512], buf[8192]; off_t offset;
    PGresult *owned_result;
} req_t;


static inline struct io_uring_sqe *_get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    return sqe ? sqe : (io_uring_submit(ring), io_uring_get_sqe(ring));
}

static inline void _rearm_read(struct io_uring *ring, int fd) {
    initial_read_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    *ctx = (initial_read_ctx_t){.type = EVENT_HTTP_READ_INITIAL, .client_fd = fd};
    
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_read(sqe, fd, ctx->buf, INITIAL_READ_BUF_SIZE - 1, 0);
    sqe->flags |= IOSQE_IO_LINK; 
    io_uring_sqe_set_data(sqe, ctx);

    static struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 6000000000};
    sqe = _get_sqe(ring);
    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_submit(ring);
}

static inline void send_res_bin(struct io_uring *ring, int fd, const char *s, const char *t, const char *b, size_t n) {
    req_t *r = calloc(1, sizeof(*r)); 
    if (!r) return;

    *r = (req_t){.core_type = EVENT_HTTP_HELPER, .type = OP_WRITE_RES, .ring = ring, .client_fd = fd};

    const char *payload = b;
    size_t payload_len = n;

    if (b && n) {
        if (n >= sizeof(r->buf)) {
            payload = "{\"error\":\"response too large\"}";
            payload_len = strlen(payload);
        } else {
            memcpy(r->buf, b, n);
            r->buf[n] = '\0';
            payload = r->buf;
            payload_len = n;
        }
    }

    r->iov[0].iov_len = snprintf(r->h, 512, "HTTP/1.1 %s\r\nConnection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", s, t, payload_len);
    r->iov[0].iov_base = r->h;
    r->iov[1].iov_base = (void*)payload;
    r->iov[1].iov_len = payload_len;
    
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_writev(sqe, fd, r->iov, (b && n) ? 2 : 1, 0);
    io_uring_sqe_set_data(sqe, r); 
    io_uring_submit(ring);
}

static inline void send_res(struct io_uring *ring, int fd, const char *s, const char *t, const char *b) {
    send_res_bin(ring, fd, s, t, b, b ? strlen(b) : 0);
}

// Envia body por puntero directo (sin memcpy en user-space) y conserva el
// PGresult vivo hasta que termine el write en el CQE.
static inline void send_res_pgresult_ref(struct io_uring *ring, int fd, const char *s, const char *t, const char *body_ptr, size_t body_len, PGresult *owned_result) {
    req_t *r = calloc(1, sizeof(*r));
    if (!r) {
        if (owned_result) PQclear(owned_result);
        return;
    }

    *r = (req_t){.core_type = EVENT_HTTP_HELPER, .type = OP_WRITE_RES, .ring = ring, .client_fd = fd, .owned_result = owned_result};
    r->iov[0].iov_len = snprintf(r->h, 512, "HTTP/1.1 %s\r\nConnection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", s, t, body_len);
    r->iov[0].iov_base = r->h;
    r->iov[1].iov_base = (void *)body_ptr;
    r->iov[1].iov_len = body_len;

    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_writev(sqe, fd, r->iov, (body_ptr && body_len) ? 2 : 1, 0);
    io_uring_sqe_set_data(sqe, r);
    io_uring_submit(ring);
}

static inline void send_json_pgresult_ref(struct io_uring *ring, int fd, const char *json_ptr, size_t json_len, PGresult *owned_result) {
    send_res_pgresult_ref(ring, fd, "200 OK", "application/json", json_ptr, json_len, owned_result);
}

static inline void stream_file(struct io_uring *ring, int fd, const char *p, const char *t) {
    struct stat st; int ffd = open(p, O_RDONLY);
    if (ffd < 0) return send_res(ring, fd, "404 Not Found", "text/plain", "404");
    if (fstat(ffd, &st) < 0) return (void)(close(ffd), send_res(ring, fd, "500 Internal Error", "text/plain", "500"));
    
    req_t *r = calloc(1, sizeof(*r));
    if (!r) return (void)close(ffd);
    
    *r = (req_t){.core_type = EVENT_HTTP_HELPER, .type = OP_WRITE_HEADER, .ring = ring, .client_fd = fd, .file_fd = ffd};
    r->iov[0].iov_len = snprintf(r->h, 512, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", t, (size_t)st.st_size);
    r->iov[0].iov_base = r->h;
    
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_writev(sqe, fd, r->iov, 1, 0);
    io_uring_sqe_set_data(sqe, r); 
    io_uring_submit(ring);
}

static inline void http_helper_handle_cqe(struct io_uring_cqe *cqe) {
    req_t *r = (req_t *)io_uring_cqe_get_data(cqe);
    if (!r) return;
    
    struct io_uring *ring = r->ring; struct io_uring_sqe *sqe;
    if (cqe->res < 0) {
        if (r->type != OP_CLOSE && r->file_fd > 0) close(r->file_fd);
        if (r->type != OP_CLOSE) {
            r->type = OP_CLOSE; sqe = _get_sqe(ring);
            io_uring_prep_close(sqe, r->client_fd);
            io_uring_sqe_set_data(sqe, r); io_uring_submit(ring);
        }
        return;
    }

    switch (r->type) {
        case OP_WRITE_RES:
            _rearm_read(ring, r->client_fd);
            if (r->owned_result) PQclear(r->owned_result);
            free(r);
            break;
        case OP_WRITE_HEADER: case OP_WRITE_FILE:
            r->type = OP_READ_FILE; sqe = _get_sqe(ring);
            io_uring_prep_read(sqe, r->file_fd, r->buf, sizeof(r->buf), r->offset);
            io_uring_sqe_set_data(sqe, r); io_uring_submit(ring); break;
        case OP_READ_FILE:
            if (cqe->res == 0) { close(r->file_fd); _rearm_read(ring, r->client_fd); free(r); } 
            else {
                r->offset += cqe->res; r->type = OP_WRITE_FILE; sqe = _get_sqe(ring);
                io_uring_prep_write(sqe, r->client_fd, r->buf, cqe->res, 0);
                io_uring_sqe_set_data(sqe, r); io_uring_submit(ring);
            } break;
        case OP_CLOSE:
            if (r->owned_result) PQclear(r->owned_result);
            free(r);
            break;
        default: break;
    }
}

/* ========================================================================= *
 * JSON HTTP INTEGRATION                                                     *
 * ========================================================================= */

// Extrae el puntero al inicio del body (después del Header HTTP)
static inline const char *http_get_body(const char *request_buf) {
    const char *body = strstr(request_buf, "\r\n\r\n");
    return body ? body + 4 : NULL; 
}

// Compara un token JSMN con una clave de C (string)
static inline int json_key_eq(const char *json_str, jsmntok_t *tok, const char *key) {
    if (tok->type == JSMN_STRING && (int)strlen(key) == tok->end - tok->start &&
        strncmp(json_str + tok->start, key, tok->end - tok->start) == 0) {
        return 1;
    }
    return 0;
}

// Parsea el body de un request HTTP y devuelve los tokens
static inline int http_parse_json_body(const char *request_buf, jsmntok_t *tokens, unsigned int max_tokens, const char **out_body) {
    const char *body = http_get_body(request_buf);
    if (!body) return -1; // No se encontró body o petición malformada
    
    if (out_body) *out_body = body; // Guardamos el puntero al inicio del JSON
    
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, body, strlen(body), tokens, max_tokens);
}

#define send_html(r, fd, b) send_res(r, fd, "200 OK", "text/html; charset=utf-8", b)
#define send_json(r, fd, b) send_res(r, fd, "200 OK", "application/json", b)
#define send_text(r, fd, b) send_res(r, fd, "200 OK", "text/plain", b)
#define send_404(r, fd)     send_res(r, fd, "404 Not Found", "text/plain", "404")

#endif