#ifndef HTTP_HELPER_H
#define HTTP_HELPER_H

#include <liburing.h>
#include <linux/openat2.h>
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

typedef enum {
    OP_WRITE_RES, OP_OPEN_FILE, OP_STAT_FILE, OP_WRITE_HEADER,
    OP_READ_FILE, OP_WRITE_FILE, OP_CLOSE, OP_TIMEOUT
} req_type_t;

typedef struct {
    event_type_t core_type; req_type_t type; struct io_uring *ring;
    int client_fd, file_fd; struct iovec iov[2];
    char h[512], buf[8192]; off_t offset;
    struct statx stx; const char *mime;
    int dir_fd, spa_fallback, fallback_tried;
    PGresult *owned_result;
} req_t;

// Recuerda, por fd, si la conexion que acaba de parsearse pidio keep-alive
// (HTTP/1.1 por defecto, o header explicito) o close (HTTP/1.0 por defecto,
// o "Connection: close"). Sin esto el server prometia keep-alive siempre sin
// importar lo que pidiera el cliente, y clientes estrictos (ab, proxies viejos)
// se quedaban esperando el cierre de la conexion en vez de confiar en
// Content-Length. Se setea al parsear el request (server.c) y se lee al
// escribir la respuesta y al decidir si rearmar el read o cerrar el fd.
// extern (no static): http.h se incluye desde varios .c (server.c,
// controllers/sakila.c, ...) y cada uno necesita ver el MISMO arreglo por
// hilo. Con "static" cada translation unit tenia su propia copia privada:
// server.c marcaba keep-alive=1 al parsear, pero send_res_bin() llamado
// desde controllers/sakila.c leia una copia distinta (siempre en 0) y
// mandaba "Connection: close" aunque el cliente pidiera keep-alive —
// el cliente se quedaba esperando mas bytes que nunca llegaban.
#define MAX_TRACKED_FD 65536
extern __thread unsigned char conn_keep_alive[MAX_TRACKED_FD];

static inline void set_keep_alive(int fd, int ka) {
    if (fd >= 0 && fd < MAX_TRACKED_FD) conn_keep_alive[fd] = (unsigned char)(ka ? 1 : 0);
}

static inline int get_keep_alive(int fd) {
    return (fd >= 0 && fd < MAX_TRACKED_FD) ? conn_keep_alive[fd] : 0;
}

// Defensa en profundidad: estos headers no dependen de que nginx este bien
// configurado delante. Van en toda respuesta, API o estatica.
// - nosniff: el browser no debe re-interpretar el Content-Type declarado.
// - X-Frame-Options: nada de embeber esto en un <iframe> ajeno (clickjacking).
// - Referrer-Policy: no filtrar el path completo (con posibles IDs/tokens
//   en query string) al navegar a un origen externo.
#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: DENY\r\n" \
    "Referrer-Policy: strict-origin-when-cross-origin\r\n"

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

    static struct __kernel_timespec ts = {.tv_sec = 6, .tv_nsec = 0};
    sqe = _get_sqe(ring);
    io_uring_prep_link_timeout(sqe, &ts, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_submit(ring);
}

// El cliente no pidio (o no acepta) keep-alive: cerramos el fd en vez de
// dejarlo esperando otro request que nunca va a llegar.
static inline void _close_conn(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_submit(ring);
}

// Tras escribir una respuesta: sigue en la misma conexion si el cliente
// quiere keep-alive, o la cierra si pidio close (o era HTTP/1.0 sin pedirlo).
static inline void _rearm_or_close(struct io_uring *ring, int fd) {
    if (get_keep_alive(fd)) _rearm_read(ring, fd);
    else _close_conn(ring, fd);
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

    r->iov[0].iov_len = snprintf(r->h, 512, "HTTP/1.1 %s\r\nConnection: %s\r\n" SECURITY_HEADERS "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n", s, get_keep_alive(fd) ? "keep-alive" : "close", t, payload_len);
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
    r->iov[0].iov_len = snprintf(r->h, 512, "HTTP/1.1 %s\r\nConnection: %s\r\n" SECURITY_HEADERS "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n", s, get_keep_alive(fd) ? "keep-alive" : "close", t, body_len);
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

// Sirve un archivo de forma totalmente asincrona: abre con openat2 (RESOLVE_BENEATH
// obliga al kernel a rechazar cualquier resolucion que escape de root_fd, sea por
// ".." o por symlinks, de forma atomica — sin el TOCTOU de un realpath()+open() separados),
// pide el tamano con statx y encadena el mismo ciclo read/write por chunks que ya
// exisitia. Ningun syscall de esta ruta bloquea al worker.
// spa_fallback: si el archivo pedido no existe, en vez de 404 se reintenta
// abrir "index.html" en la misma raiz — asi el router client-side de Svelte
// recibe la app y decide el resto (rutas como /about, /users/42, etc.).
static inline void serve_file_async(struct io_uring *ring, int client_fd, int root_fd, const char *rel_path, const char *mime, int spa_fallback) {
    req_t *r = calloc(1, sizeof(*r));
    if (!r) return (void)send_res(ring, client_fd, "500 Internal Error", "text/plain", "500");

    *r = (req_t){.core_type = EVENT_HTTP_HELPER, .type = OP_OPEN_FILE, .ring = ring, .client_fd = client_fd,
                 .mime = mime, .dir_fd = root_fd, .spa_fallback = spa_fallback};

    struct open_how how = { .flags = O_RDONLY, .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS };
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_openat2(sqe, root_fd, rel_path, &how);
    io_uring_sqe_set_data(sqe, r);
    io_uring_submit(ring);
}

static inline void http_helper_handle_cqe(struct io_uring_cqe *cqe) {
    req_t *r = (req_t *)io_uring_cqe_get_data(cqe);
    if (!r) return;
    
    struct io_uring *ring = r->ring; struct io_uring_sqe *sqe;
    if (cqe->res < 0) {
        // Todavia no le mandamos nada al cliente en estos dos estados (openat2
        // rechazado por RESOLVE_BENEATH, archivo inexistente, o statx fallido):
        // vale la pena responder 404 real en vez de solo cortar la conexion.
        if (r->type == OP_OPEN_FILE) {
            if (r->spa_fallback && !r->fallback_tried) {
                r->fallback_tried = 1;
                r->mime = "text/html; charset=utf-8";
                struct open_how how = { .flags = O_RDONLY, .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS };
                sqe = _get_sqe(ring);
                io_uring_prep_openat2(sqe, r->dir_fd, "index.html", &how);
                io_uring_sqe_set_data(sqe, r);
                io_uring_submit(ring);
                return;
            }
            send_res(ring, r->client_fd, "404 Not Found", "text/plain", "404");
            free(r);
            return;
        }
        if (r->type == OP_STAT_FILE) {
            close(r->file_fd);
            send_res(ring, r->client_fd, "404 Not Found", "text/plain", "404");
            free(r);
            return;
        }
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
            _rearm_or_close(ring, r->client_fd);
            if (r->owned_result) PQclear(r->owned_result);
            free(r);
            break;
        case OP_OPEN_FILE:
            r->file_fd = cqe->res;
            r->type = OP_STAT_FILE;
            sqe = _get_sqe(ring);
            io_uring_prep_statx(sqe, r->file_fd, "", AT_EMPTY_PATH, STATX_SIZE | STATX_TYPE, &r->stx);
            io_uring_sqe_set_data(sqe, r);
            io_uring_submit(ring);
            break;
        case OP_STAT_FILE:
            if (!S_ISREG(r->stx.stx_mode)) {
                close(r->file_fd);
                send_res(ring, r->client_fd, "404 Not Found", "text/plain", "404");
                free(r);
                break;
            }
            r->type = OP_WRITE_HEADER;
            r->iov[0].iov_len = snprintf(r->h, 512,
                "HTTP/1.1 200 OK\r\nConnection: %s\r\n" SECURITY_HEADERS "Content-Type: %s\r\nContent-Length: %llu\r\n\r\n",
                get_keep_alive(r->client_fd) ? "keep-alive" : "close", r->mime, (unsigned long long)r->stx.stx_size);
            r->iov[0].iov_base = r->h;
            sqe = _get_sqe(ring);
            io_uring_prep_writev(sqe, r->client_fd, r->iov, 1, 0);
            io_uring_sqe_set_data(sqe, r);
            io_uring_submit(ring);
            break;
        case OP_WRITE_HEADER: case OP_WRITE_FILE:
            r->type = OP_READ_FILE; sqe = _get_sqe(ring);
            io_uring_prep_read(sqe, r->file_fd, r->buf, sizeof(r->buf), r->offset);
            io_uring_sqe_set_data(sqe, r); io_uring_submit(ring); break;
        case OP_READ_FILE:
            if (cqe->res == 0) { close(r->file_fd); _rearm_or_close(ring, r->client_fd); free(r); }
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