#ifndef HTTP_HELPER_H
#define HTTP_HELPER_H
#include <uv.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h> // Necesario para O_RDONLY

// --- 1. MANEJO EN MEMORIA (Archivos chicos, HTML, JSON) ---
typedef struct { uv_write_t w; char h[512]; } req_t;

static void on_w(uv_write_t *r, int s) {
    !uv_is_closing((uv_handle_t*)r->handle) ? uv_close((uv_handle_t*)r->handle, (uv_close_cb)free) : (void)0;
    free(r);
}

static inline void send_res_bin(uv_stream_t *c, const char *st, const char *ct, const char *bd, size_t len) {
    req_t *r = malloc(sizeof(*r));
    if (r) {
        int hl = snprintf(r->h, sizeof(r->h), "HTTP/1.1 %s\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", st, ct, len);
        uv_buf_t bufs[2] = { uv_buf_init(r->h, hl < 512 ? hl : 511), uv_buf_init((char*)bd, len) };
        uv_write((uv_write_t*)r, c, bufs, bd && len > 0 ? 2 : 1, on_w);
    }
}

static inline void send_res(uv_stream_t *c, const char *st, const char *ct, const char *bd) {
    send_res_bin(c, st, ct, bd, bd ? strlen(bd) : 0);
}

// --- 2. MAGIA SENDFILE (Streaming Zero-Copy para archivos GIGANTES) ---
typedef struct {
    uv_write_t w; uv_fs_t req;
    uv_file fd; uv_os_fd_t sock;
    int64_t off; size_t sz; char h[512];
} file_req_t;

static void on_fs(uv_fs_t *req) {
    file_req_t *r = req->data;
    // Si mandó un chunk y aún falta, lo volvemos a llamar. Si terminó (o falló), cerramos todo.
    req->result > 0 && (r->off += req->result) < r->sz ? 
        (uv_fs_req_cleanup(req), uv_fs_sendfile(req->loop, req, (uv_file)r->sock, r->fd, r->off, r->sz - r->off, on_fs)) :
        (uv_fs_req_cleanup(req), uv_fs_close(req->loop, req, r->fd, NULL), !uv_is_closing((uv_handle_t*)r->w.handle) ? uv_close((uv_handle_t*)r->w.handle, (uv_close_cb)free) : free(r));
}

static void on_h(uv_write_t *w, int s) {
    file_req_t *r = (file_req_t*)w; r->req.data = r;
    // Iniciamos la transferencia directa OS->Red solo si los headers se mandaron bien
    s == 0 ? uv_fs_sendfile(w->handle->loop, &r->req, (uv_file)r->sock, r->fd, r->off, r->sz, on_fs) : 
             (uv_fs_close(w->handle->loop, &r->req, r->fd, NULL), !uv_is_closing((uv_handle_t*)w->handle) ? uv_close((uv_handle_t*)w->handle, (uv_close_cb)free) : free(r));
}

static inline void stream_file(uv_stream_t *c, const char *path, const char *ct) {
    uv_fs_t op, st; 
    uv_file fd = uv_fs_open(c->loop, &op, path, O_RDONLY, 0, NULL);
    if (fd < 0) { send_res(c, "404 Not Found", "text/plain", "Archivo no encontrado"); return; }
    uv_fs_stat(c->loop, &st, path, NULL); // Sacamos el peso del archivo
    
    file_req_t *r = malloc(sizeof(*r));
    if (r) {
        r->fd = fd; r->sz = st.statbuf.st_size; r->off = 0;
        uv_fileno((uv_handle_t*)c, &r->sock); // Extraemos el FD nativo del socket de libuv
        int hl = snprintf(r->h, sizeof(r->h), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n", ct, r->sz);
        uv_write(&r->w, c, &(uv_buf_t){r->h, hl < 512 ? hl : 511}, 1, on_h); // 1ro mandamos headers
    } else uv_fs_close(c->loop, &op, fd, NULL);
    
    uv_fs_req_cleanup(&op); uv_fs_req_cleanup(&st);
}

// --- MACROS NINJA PARA TUS CONTROLADORES ---
#define send_html(c, b) send_res(c, "200 OK", "text/html; charset=utf-8", b)
#define send_json(c, b) send_res(c, "200 OK", "application/json; charset=utf-8", b)
#define send_text(c, b) send_res(c, "200 OK", "text/plain; charset=utf-8", b)
#define send_404(c)     send_res(c, "404 Not Found", "text/plain; charset=utf-8", "404")

#endif
