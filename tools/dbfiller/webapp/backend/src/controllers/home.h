/*
 * controllers/home.h - healthcheck y handler generico de 404
 *
 * DESCRIPCION
 *     Trimeado para dbfiller-web: la version original del scaffold traia
 *     ademas 'api' (ejemplo de query a Postgres via el pool del
 *     framework) y 'echo' (ejemplo de parametro de ruta) -- ninguno de
 *     los dos aplica aca (dbfiller-web no usa el pool del framework, ver
 *     el comentario grande en config/db.c, init_db(); y no necesita un
 *     endpoint de ejemplo). Se dejan solo healthz (usado como
 *     healthcheck del contenedor) y error404 (usado por dispatch() en
 *     utils/http/router.h como fallback generico).
 */
#pragma once
#include <liburing.h>

/*
 * healthz - handler de GET /healthz
 *
 * No toca la base de datos: responde 200 en cuanto el hilo que recibio
 * la conexion pudo parsear el request. Pensado para que un orquestador
 * (Docker, k8s) distinga "el proceso esta vivo y atendiendo conexiones"
 * de cualquier otra cosa.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso (la ruta ya filtro por GET)
 *   b - cuerpo del request, sin uso
 */
static inline void healthz(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    /* send_res esta definida en utils/http/http.h; se declara aqui por
       el mismo motivo que antes: este header se incluye antes que
       router.h en el orden de compilacion. */
    extern void send_res(struct io_uring*, int, const char*, const char*, const char*);
    send_res(r, f, "200 OK", "text/plain", "ok");
}

/*
 * error404 - handler generico invocado cuando ninguna ruta hace match
 *
 * Usado directamente por dispatch() en utils/http/router.h cuando un
 * path bajo /api no coincide con ninguna ruta registrada.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso
 *   b - cuerpo del request, sin uso
 */
inline void error404(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    extern void send_res(struct io_uring*, int, const char*, const char*, const char*);
    send_res(r, f, "404 Not Found", "text/plain", "404");
}
