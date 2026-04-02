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
#include "events.h"

#define INITIAL_READ_BUF_SIZE 4096

typedef enum {
    OP_WRITE_RES,
    OP_WRITE_HEADER,
    OP_READ_FILE,
    OP_WRITE_FILE,
    OP_CLOSE,
    OP_TIMEOUT // Nuevo tipo para identificar el evento de tiempo
} req_type_t;

typedef struct {
    event_type_t core_type; 
    req_type_t type;
    struct io_uring *ring;
    int client_fd;
    int file_fd;
    struct iovec iov[2];
    char h[512];
    char buf[8192]; 
    off_t offset;
} req_t;

typedef struct {
    event_type_t type;
    int client_fd;
    char buf[INITIAL_READ_BUF_SIZE];
} initial_read_ctx_t;

// Estructura estática para el timeout (500ms)
static struct __kernel_timespec timeout_ts = {
    .tv_sec = 0,
    .tv_nsec = 6000000000 // 500 millones de nanosegundos = 500ms
};

static inline struct io_uring_sqe *_get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

// Re-armar lectura con LINKED TIMEOUT de 500ms
static inline void _rearm_read(struct io_uring *ring, int fd) {
    initial_read_ctx_t *read_ctx = calloc(1, sizeof(*read_ctx));
    read_ctx->type = EVENT_HTTP_READ_INITIAL;
    read_ctx->client_fd = fd;
    
    // 1. Preparar el READ
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_read(sqe, fd, read_ctx->buf, INITIAL_READ_BUF_SIZE - 1, 0);
    sqe->flags |= IOSQE_IO_LINK; // ENLAZAR con el siguiente SQE
    io_uring_sqe_set_data(sqe, read_ctx);

    // 2. Preparar el TIMEOUT (se ejecutará si el READ no termina)
    sqe = _get_sqe(ring);
    // IORING_TIMEOUT_ABS no, usaremos relativo (0)
    io_uring_prep_timeout(sqe, &timeout_ts, 0, 0);
    
    // Usamos un puntero dummy o nulo para el timeout, ya que el fallo del READ 
    // será capturado por el core_type del read_ctx
    io_uring_sqe_set_data(sqe, NULL); 
    
    io_uring_submit(ring);
}

static inline void send_res_bin(struct io_uring *ring, int fd, const char *s, const char *t, const char *b, size_t n) {
    req_t *r = calloc(1, sizeof(*r));
    if (!r) return;
    
    r->core_type = EVENT_HTTP_HELPER;
    r->type = OP_WRITE_RES;
    r->ring = ring;
    r->client_fd = fd;
    
    int l = snprintf(r->h, 512, "HTTP/1.1 %s\r\nConnection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", s, t, n);
    
    r->iov[0].iov_base = r->h;
    r->iov[0].iov_len = l;
    r->iov[1].iov_base = (void*)b;
    r->iov[1].iov_len = n;

    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_writev(sqe, fd, r->iov, b && n ? 2 : 1, 0);
    io_uring_sqe_set_data(sqe, r);
    io_uring_submit(ring);
}

static inline void send_res(struct io_uring *ring, int fd, const char *s, const char *t, const char *b) {
    send_res_bin(ring, fd, s, t, b, b ? strlen(b) : 0);
}

static inline void stream_file(struct io_uring *ring, int fd, const char *p, const char *t) {
    int ffd = open(p, O_RDONLY);
    if (ffd < 0) return send_res(ring, fd, "404 Not Found", "text/plain", "404");
    
    struct stat st;
    if (fstat(ffd, &st) < 0) {
        close(ffd);
        return send_res(ring, fd, "500 Internal Error", "text/plain", "500");
    }

    req_t *r = calloc(1, sizeof(*r));
    if (r) {
        r->core_type = EVENT_HTTP_HELPER;
        r->type = OP_WRITE_HEADER;
        r->ring = ring;
        r->client_fd = fd;
        r->file_fd = ffd;
        r->offset = 0;
        
        int l = snprintf(r->h, 512, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", t, (size_t)st.st_size);
        r->iov[0].iov_base = r->h;
        r->iov[0].iov_len = l;
        
        struct io_uring_sqe *sqe = _get_sqe(ring);
        io_uring_prep_writev(sqe, fd, r->iov, 1, 0);
        io_uring_sqe_set_data(sqe, r);
        io_uring_submit(ring);
    } else {
        close(ffd);
    }
}

static inline void http_helper_handle_cqe(struct io_uring_cqe *cqe) {
    // Si el data es NULL, es el CQE del timeout que se completó o canceló
    if (!io_uring_cqe_get_data(cqe)) return;

    req_t *r = (req_t *)io_uring_cqe_get_data(cqe);
    struct io_uring *ring = r->ring;

    // Si cqe->res < 0, incluye el caso de timeout expirado (-ETIME)
    if (cqe->res < 0) {
        if (r->type != OP_CLOSE && r->file_fd > 0) close(r->file_fd);
        goto force_close;
    }

    switch (r->type) {
        case OP_WRITE_RES:
            _rearm_read(ring, r->client_fd);
            free(r);
            break;

        case OP_WRITE_HEADER:
        case OP_WRITE_FILE:
            {
                struct io_uring_sqe *sqe = _get_sqe(ring);
                r->type = OP_READ_FILE;
                io_uring_prep_read(sqe, r->file_fd, r->buf, sizeof(r->buf), r->offset);
                io_uring_sqe_set_data(sqe, r);
                io_uring_submit(ring);
            }
            break;

        case OP_READ_FILE:
            if (cqe->res == 0) { 
                close(r->file_fd);
                r->file_fd = -1;
                _rearm_read(ring, r->client_fd);
                free(r);
            } else {
                int bytes_read = cqe->res;
                r->offset += bytes_read;
                struct io_uring_sqe *sqe = _get_sqe(ring);
                r->type = OP_WRITE_FILE;
                io_uring_prep_write(sqe, r->client_fd, r->buf, bytes_read, 0);
                io_uring_sqe_set_data(sqe, r);
                io_uring_submit(ring);
            }
            break;

        case OP_CLOSE:
            free(r);
            break;
    }
    return;

force_close:
    if (r->type != OP_CLOSE) {
        struct io_uring_sqe *sqe = _get_sqe(ring);
        r->type = OP_CLOSE;
        io_uring_prep_close(sqe, r->client_fd);
        io_uring_sqe_set_data(sqe, r);
        io_uring_submit(ring);
    }
}

#define send_html(ring, fd, b) send_res(ring, fd, "200 OK", "text/html; charset=utf-8", b)
#define send_json(ring, fd, b) send_res(ring, fd, "200 OK", "application/json", b)
#define send_text(ring, fd, b) send_res(ring, fd, "200 OK", "text/plain", b)
#define send_404(ring, fd)     send_res(ring, fd, "404 Not Found", "text/plain", "404")

#endif