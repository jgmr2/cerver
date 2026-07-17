/*
 * utils/http/http.h - helper HTTP asincrono: respuestas, archivos y body JSON
 *
 * NOMBRE
 *     http.h - envio de respuestas, servido de archivos por io_uring y
 *     extraccion/parseo del body de un request
 *
 * DESCRIPCION
 *     Concentra todo el I/O de bajo nivel que necesita el resto del
 *     servidor para hablar HTTP sobre io_uring:
 *       - Seguimiento de keep-alive por file descriptor (conn_keep_alive).
 *       - Envio de respuestas completas en memoria (send_res / send_json
 *         / macros send_html, send_text, send_404).
 *       - Servido de archivos por chunks, totalmente asincrono, con
 *         fallback SPA (serve_file_async).
 *       - Extraccion del body de un request ya parseado y su tokenizado
 *         JSON (http_get_body, http_parse_json_body, sobre utils/json.h).
 *
 *     Todas las operaciones de este archivo viajan por el mismo anillo
 *     io_uring del hilo: se arma un SQE, se somete, y la continuacion se
 *     resuelve en http_helper_handle_cqe() cuando llega el CQE
 *     correspondiente (invocada desde core/server.c para todo evento
 *     EVENT_HTTP_HELPER).
 */
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
#include "../json.h"

/* Tamano del buffer de lectura inicial de cada conexion (ver
 * initial_read_ctx_t en utils/events.h y su uso en core/server.c). */
#define INITIAL_READ_BUF_SIZE 4096

/*
 * req_type_t - fase en la que esta un req_t dentro de su propio pipeline
 *
 *   OP_WRITE_RES    - escribiendo una respuesta ya armada en memoria
 *   OP_OPEN_FILE    - esperando el resultado de openat2 (static.h)
 *   OP_STAT_FILE    - esperando el resultado de statx sobre el fd abierto
 *   OP_WRITE_HEADER - escribiendo la cabecera HTTP antes del cuerpo del archivo
 *   OP_READ_FILE    - leyendo el siguiente chunk del archivo
 *   OP_WRITE_FILE   - escribiendo el chunk leido al socket del cliente
 *   OP_CLOSE        - cerrando el fd del cliente tras un error a mitad de pipeline
 *   OP_TIMEOUT      - reservado (el link_timeout de _rearm_read no pasa por aqui)
 */
typedef enum {
    OP_WRITE_RES, OP_OPEN_FILE, OP_STAT_FILE, OP_WRITE_HEADER,
    OP_READ_FILE, OP_WRITE_FILE, OP_CLOSE, OP_TIMEOUT
} req_type_t;

/*
 * req_t - contexto de una operacion HTTP en curso sobre io_uring
 *
 * Un mismo req_t viaja por varias operaciones encadenadas (por ejemplo
 * OP_OPEN_FILE -> OP_STAT_FILE -> OP_WRITE_HEADER -> OP_READ_FILE ->
 * OP_WRITE_FILE -> ... -> OP_READ_FILE con cqe->res==0), mutando su
 * campo 'type' en cada paso; se libera (free) en el ultimo paso de cada
 * pipeline (ver http_helper_handle_cqe).
 *
 * Campos:
 *   core_type     - siempre EVENT_HTTP_HELPER (ver utils/events.h)
 *   type          - fase actual (req_type_t)
 *   ring          - anillo io_uring del hilo dueno de esta operacion
 *   client_fd     - fd del cliente que recibe la respuesta
 *   file_fd       - fd del archivo abierto (solo en el pipeline de estaticos)
 *   iov[2]        - vectores de escritura (cabecera + cuerpo, o chunk de archivo)
 *   h             - buffer de la cabecera HTTP generada
 *   buf           - buffer de chunk de archivo (lectura y escritura)
 *   offset        - posicion actual dentro del archivo servido
 *   stx           - resultado de statx (tamano y tipo del archivo)
 *   mime          - Content-Type a usar en la respuesta de archivo
 *   dir_fd        - fd del directorio raiz del mount (para el fallback SPA)
 *   spa_fallback  - si el archivo pedido no existe, reintentar index.html
 *   fallback_tried - evita reintentar el fallback SPA mas de una vez
 *   owned_result  - PGresult que este req_t mantiene vivo hasta terminar
 *                   de escribirlo (ver send_res_pgresult_ref)
 *   heap_body     - copia del body reservada con malloc cuando no entra
 *                   en buf (ver send_res_bin); NULL si el body uso el
 *                   buffer inline o no habia body. Se libera junto con
 *                   owned_result al terminar el write (o al cerrar tras
 *                   un error de escritura).
 */
typedef struct {
    event_type_t core_type; req_type_t type; struct io_uring *ring;
    int client_fd, file_fd; struct iovec iov[2];
    char h[512], buf[8192]; off_t offset;
    struct statx stx; const char *mime;
    int dir_fd, spa_fallback, fallback_tried;
    PGresult *owned_result;
    char *heap_body;
} req_t;

/*
 * conn_keep_alive - estado de keep-alive por file descriptor, por hilo
 *
 * Recuerda, por fd, si la conexion que acaba de parsearse pidio
 * keep-alive (HTTP/1.1 por defecto, o header explicito) o close
 * (HTTP/1.0 por defecto, o "Connection: close"). Sin esto el server
 * prometia keep-alive siempre sin importar lo que pidiera el cliente, y
 * clientes estrictos (ab, proxies viejos) se quedaban esperando el
 * cierre de la conexion en vez de confiar en Content-Length. Se setea al
 * parsear el request (core/server.c) y se lee al escribir la respuesta
 * y al decidir si rearmar el read o cerrar el fd.
 *
 * Se declara extern (no static) a proposito: este header se incluye
 * desde varios .c (core/server.c, controllers/sakila.c, ...) y cada uno
 * necesita ver el MISMO arreglo por hilo. Con "static" cada translation
 * unit tendria su propia copia privada: core/server.c marcaria
 * keep-alive=1 al parsear, pero send_res_bin() llamado desde otro .c
 * leeria una copia distinta (siempre en 0) y mandaria "Connection:
 * close" aunque el cliente pidiera keep-alive, dejandolo esperando mas
 * bytes que nunca llegan. La definicion real (sin extern) vive en
 * utils/http/http.c.
 */
#define MAX_TRACKED_FD 65536
extern __thread unsigned char conn_keep_alive[MAX_TRACKED_FD];

/*
 * set_keep_alive - marca el estado de keep-alive de un fd
 *
 * Parametros:
 *   fd - file descriptor de la conexion (ignorado si esta fuera de rango)
 *   ka - distinto de 0 para keep-alive, 0 para close
 */
static inline void set_keep_alive(int fd, int ka) {
    if (fd >= 0 && fd < MAX_TRACKED_FD) conn_keep_alive[fd] = (unsigned char)(ka ? 1 : 0);
}

/*
 * get_keep_alive - consulta el estado de keep-alive de un fd
 *
 * Parametros:
 *   fd - file descriptor de la conexion
 *
 * Retorna:
 *   1 si el fd esta marcado keep-alive, 0 si esta marcado close o fuera
 *   de rango.
 */
static inline int get_keep_alive(int fd) {
    return (fd >= 0 && fd < MAX_TRACKED_FD) ? conn_keep_alive[fd] : 0;
}

/*
 * SECURITY_HEADERS - cabeceras de seguridad enviadas en toda respuesta
 *
 * Defensa en profundidad: estos headers no dependen de que nginx (u
 * otro proxy) este bien configurado delante.
 *   - nosniff: el browser no debe re-interpretar el Content-Type declarado.
 *   - X-Frame-Options: nada de embeber esto en un iframe ajeno (clickjacking).
 *   - Referrer-Policy: no filtrar el path completo (con posibles IDs o
 *     tokens en query string) al navegar a un origen externo.
 */
#define SECURITY_HEADERS \
    "X-Content-Type-Options: nosniff\r\n" \
    "X-Frame-Options: DENY\r\n" \
    "Referrer-Policy: strict-origin-when-cross-origin\r\n"

/*
 * _get_sqe - obtiene un SQE libre, sometiendo si hace falta
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *
 * Retorna:
 *   un SQE listo para usar (somete lo ya encolado y reintenta una vez si
 *   io_uring_get_sqe devuelve NULL por falta de espacio).
 */
static inline struct io_uring_sqe *_get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    return sqe ? sqe : (io_uring_submit(ring), io_uring_get_sqe(ring));
}

/*
 * _rearm_read - vuelve a armar la lectura inicial de una conexion
 *
 * Reserva un initial_read_ctx_t nuevo y encadena un read con un
 * link_timeout de 6 segundos (IOSQE_IO_LINK): si el cliente no manda
 * nada dentro de ese plazo, el timeout cancela el read y la conexion se
 * cierra. Esto es lo que evita que un cliente tipo slowloris (abre la
 * conexion y no manda nunca el request) deje el fd colgado para
 * siempre: sin este timeout no hay ningun otro punto en el codigo que
 * lo cierre.
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *   fd   - file descriptor de la conexion a releer
 */
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

/*
 * _close_conn - cierra una conexion que no sigue en keep-alive
 *
 * El cliente no pidio (o no acepta) keep-alive: se cierra el fd en vez
 * de dejarlo esperando otro request que nunca va a llegar.
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *   fd   - file descriptor a cerrar
 */
static inline void _close_conn(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = _get_sqe(ring);
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_submit(ring);
}

/*
 * _rearm_or_close - decide que hacer con la conexion tras mandar una respuesta
 *
 * Sigue en la misma conexion (releyendo el siguiente request) si el
 * cliente quiere keep-alive, o la cierra si pidio close (o era HTTP/1.0
 * sin pedirlo explicitamente).
 *
 * Durante el apagado graceful (g_shutdown, ver utils/events.h) se cierra
 * siempre, sin importar keep-alive: es lo que hace que las conexiones
 * existentes se vayan drenando solas a medida que cada una termina su
 * request en curso, en vez de quedar reabriendo lecturas indefinidamente
 * mientras el hilo intenta salir.
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *   fd   - file descriptor de la conexion recien atendida
 */
static inline void _rearm_or_close(struct io_uring *ring, int fd) {
    if (!g_shutdown && get_keep_alive(fd)) _rearm_read(ring, fd);
    else _close_conn(ring, fd);
}

/*
 * send_res_bin - arma y envia una respuesta HTTP completa con body binario
 *
 * Copia el body a un buffer propio del req_t (hasta 8 KB, sin reservar
 * memoria) para que siga vivo hasta que termine el writev asincrono; un
 * body mas grande se copia a un buffer aparte con malloc (r->heap_body,
 * liberado en http_helper_handle_cqe junto con owned_result) en vez de
 * descartarse: endpoints que arman resultados grandes (p.ej listados con
 * muchas filas o columnas anchas, ver tools/dbfiller) no tienen por que
 * toparse con un limite arbitrario de 8 KB.
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *   fd   - file descriptor del cliente
 *   s    - linea de estado HTTP (p.ej. "200 OK", "404 Not Found")
 *   t    - Content-Type de la respuesta
 *   b    - body de la respuesta (puede ser NULL)
 *   n    - longitud de b en bytes
 */
static inline void send_res_bin(struct io_uring *ring, int fd, const char *s, const char *t, const char *b, size_t n) {
    req_t *r = calloc(1, sizeof(*r));
    if (!r) return;

    *r = (req_t){.core_type = EVENT_HTTP_HELPER, .type = OP_WRITE_RES, .ring = ring, .client_fd = fd};

    const char *payload = b;
    size_t payload_len = n;

    if (b && n) {
        if (n < sizeof(r->buf)) {
            memcpy(r->buf, b, n);
            r->buf[n] = '\0';
            payload = r->buf;
            payload_len = n;
        } else {
            char *heap = malloc(n + 1);
            if (heap) {
                memcpy(heap, b, n);
                heap[n] = '\0';
                r->heap_body = heap;
                payload = heap;
                payload_len = n;
            } else {
                payload = "{\"error\":\"out of memory\"}";
                payload_len = strlen(payload);
            }
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

/*
 * send_res - arma y envia una respuesta HTTP completa con body de texto
 *
 * Envoltorio de send_res_bin usando strlen(b) como longitud.
 *
 * Parametros:
 *   ring - anillo io_uring del hilo actual
 *   fd   - file descriptor del cliente
 *   s    - linea de estado HTTP
 *   t    - Content-Type de la respuesta
 *   b    - body de texto terminado en NUL (puede ser NULL)
 */
static inline void send_res(struct io_uring *ring, int fd, const char *s, const char *t, const char *b) {
    send_res_bin(ring, fd, s, t, b, b ? strlen(b) : 0);
}

/*
 * send_res_pgresult_ref - envia una respuesta apuntando directo a un PGresult
 *
 * Envia el body por puntero directo (sin memcpy en user-space) y
 * conserva el PGresult vivo hasta que termine el write en el CQE
 * (ver OP_WRITE_RES en http_helper_handle_cqe, que hace PQclear al
 * terminar). Si la reserva del contexto falla, libera el PGresult de
 * inmediato para no fugarlo.
 *
 * Parametros:
 *   ring          - anillo io_uring del hilo actual
 *   fd            - file descriptor del cliente
 *   s             - linea de estado HTTP
 *   t             - Content-Type de la respuesta
 *   body_ptr      - puntero al body (memoria propiedad de owned_result)
 *   body_len      - longitud del body en bytes
 *   owned_result  - PGresult cuyo ciclo de vida se extiende hasta el
 *                   final del write; esta funcion toma su propiedad
 */
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

/*
 * send_json_pgresult_ref - send_res_pgresult_ref con status/Content-Type fijos
 *
 * Envoltorio para responder 200 OK / application/json apuntando directo
 * a un PGresult (ver send_res_pgresult_ref).
 */
static inline void send_json_pgresult_ref(struct io_uring *ring, int fd, const char *json_ptr, size_t json_len, PGresult *owned_result) {
    send_res_pgresult_ref(ring, fd, "200 OK", "application/json", json_ptr, json_len, owned_result);
}

/*
 * serve_file_async - sirve un archivo de forma totalmente asincrona
 *
 * Abre con openat2 usando RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS: el
 * kernel rechaza de forma atomica cualquier resolucion que escape de
 * root_fd, sea por ".." o por symlinks (sin el TOCTOU de un
 * realpath()+open() hechos por separado). El resto del pipeline (pedir
 * el tamano con statx, encadenar el ciclo read/write por chunks) sigue
 * en http_helper_handle_cqe(). Ningun syscall de esta ruta bloquea al
 * worker.
 *
 * Parametros:
 *   ring         - anillo io_uring del hilo actual
 *   client_fd    - file descriptor del cliente
 *   root_fd      - fd de directorio (O_PATH) del mount, ver static.h
 *   rel_path     - path relativo a root_fd (sin '/' inicial)
 *   mime         - Content-Type a usar si el archivo se sirve con exito
 *   spa_fallback - si el archivo pedido no existe, en vez de 404 se
 *                  reintenta abrir "index.html" en la misma raiz, para
 *                  que el router client-side de Svelte reciba la app y
 *                  decida el resto (rutas como /about, /users/42, etc.)
 */
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

/*
 * http_helper_handle_cqe - continua el pipeline de un req_t tras un CQE
 *
 * Invocada desde core/server.c para todo CQE con tipo EVENT_HTTP_HELPER.
 * En error (cqe->res < 0):
 *   - OP_OPEN_FILE: si hay fallback SPA pendiente, reintenta con
 *     index.html; si no, responde 404.
 *   - OP_STAT_FILE: cierra el fd abierto y responde 404.
 *   - cualquier otra fase: cierra el fd del cliente (encadenando por
 *     OP_CLOSE si hiciera falta cerrar tambien el file_fd primero).
 * En exito, avanza segun 'type':
 *   OP_WRITE_RES    -> _rearm_or_close, libera owned_result/heap_body si tenia, free(r)
 *   OP_OPEN_FILE    -> pide statx sobre el fd recien abierto
 *   OP_STAT_FILE    -> si no es archivo regular, 404; si no, arma y
 *                      escribe la cabecera HTTP (OP_WRITE_HEADER)
 *   OP_WRITE_HEADER / OP_WRITE_FILE -> pide el siguiente chunk (OP_READ_FILE)
 *   OP_READ_FILE    -> si ya no hay mas datos (res==0), cierra el
 *                      archivo y decide keep-alive/close; si hay datos,
 *                      los escribe al cliente (OP_WRITE_FILE)
 *   OP_CLOSE        -> libera owned_result/heap_body si tenia, free(r)
 *
 * Parametros:
 *   cqe - CQE recibido del anillo io_uring
 */
static inline void http_helper_handle_cqe(struct io_uring_cqe *cqe) {
    req_t *r = (req_t *)io_uring_cqe_get_data(cqe);
    if (!r) return;

    struct io_uring *ring = r->ring; struct io_uring_sqe *sqe;
    if (cqe->res < 0) {
        /* Todavia no le mandamos nada al cliente en estos dos estados
         * (openat2 rechazado por RESOLVE_BENEATH, archivo inexistente, o
         * statx fallido): vale la pena responder 404 real en vez de solo
         * cortar la conexion. */
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
            free(r->heap_body);
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
            free(r->heap_body);
            free(r);
            break;
        default: break;
    }
}

/* ========================================================================= *
 * INTEGRACION HTTP + JSON                                                  *
 * ========================================================================= */

/*
 * http_get_body - ubica el inicio del body dentro de un request crudo
 *
 * Parametros:
 *   request_buf - buffer con el request completo (headers + body)
 *
 * Retorna:
 *   puntero al primer byte despues de la linea en blanco que separa
 *   headers de body, o NULL si no se encontro esa separacion.
 */
static inline const char *http_get_body(const char *request_buf) {
    const char *body = strstr(request_buf, "\r\n\r\n");
    return body ? body + 4 : NULL;
}

/*
 * json_key_eq - compara un token JSMN de tipo string contra una clave en C
 *
 * Parametros:
 *   json_str - string JSON original sobre el que apunta el token
 *   tok      - token a comparar (debe ser JSMN_STRING para poder matchear)
 *   key      - clave en C (terminada en NUL) a comparar
 *
 * Retorna:
 *   distinto de 0 si el token es un string y su contenido es exactamente
 *   igual a key
 */
static inline int json_key_eq(const char *json_str, jsmntok_t *tok, const char *key) {
    if (tok->type == JSMN_STRING && (int)strlen(key) == tok->end - tok->start &&
        strncmp(json_str + tok->start, key, tok->end - tok->start) == 0) {
        return 1;
    }
    return 0;
}

/*
 * http_parse_json_body - ubica y tokeniza el body JSON de un request
 *
 * Parametros:
 *   request_buf - buffer con el request completo
 *   tokens      - arreglo de salida para los tokens (ver utils/json.h)
 *   max_tokens  - capacidad de tokens
 *   out_body    - si no es NULL, se guarda ahi el puntero al inicio del
 *                 body (necesario para interpretar los offsets de los
 *                 tokens con json_key_eq o strncmp directo)
 *
 * Retorna:
 *   lo mismo que jsmn_parse: cantidad de tokens en exito, o -1 si no se
 *   encontro body, o un codigo negativo de jsmnerr si el JSON es
 *   invalido o incompleto.
 */
static inline int http_parse_json_body(const char *request_buf, jsmntok_t *tokens, unsigned int max_tokens, const char **out_body) {
    const char *body = http_get_body(request_buf);
    if (!body) return -1;

    if (out_body) *out_body = body;

    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, body, strlen(body), tokens, max_tokens);
}

/* Atajos de respuesta usados por controladores y por este mismo archivo. */
#define send_html(r, fd, b) send_res(r, fd, "200 OK", "text/html; charset=utf-8", b)
#define send_json(r, fd, b) send_res(r, fd, "200 OK", "application/json", b)
#define send_text(r, fd, b) send_res(r, fd, "200 OK", "text/plain", b)
#define send_404(r, fd)     send_res(r, fd, "404 Not Found", "text/plain", "404")

#endif
