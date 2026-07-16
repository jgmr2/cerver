/*
 * utils/http/router.h - tabla de rutas y despacho de requests
 *
 * NOMBRE
 *     router.h - registro de rutas (get/post/...) y dispatch() por
 *     metodo+path exacto
 *
 * DESCRIPCION
 *     La tabla routes[] es __thread: cada hilo worker la llena una vez
 *     (init_routes(), ver routes/index.h) y la consulta en cada
 *     request. El match es por igualdad exacta de metodo y path (sin
 *     parametros de ruta ni wildcards); lo que no matchea cae en
 *     try_serve_static() (utils/http/static.h) o en error404
 *     (controllers/home.h).
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <liburing.h>
#include <string.h>
#include "http.h"   /* respuestas HTTP (send_res, send_json, etc.) */
#include "static.h" /* mount_static() / try_serve_static() */

/* Firma de un handler de ruta: (anillo, fd del cliente, metodo, cuerpo
 * del request). */
typedef void (*handler_t)(struct io_uring*, int, const char*, const char*);

/* Route - una entrada de la tabla: metodo + path exactos y su handler. */
typedef struct {
    const char *method;
    const char *path;
    handler_t h;
} Route;

#define MAX_ROUTES 128
static __thread Route routes[MAX_ROUTES];
static __thread int route_count = 0;

/*
 * add_route - agrega una entrada a la tabla de rutas del hilo actual
 *
 * Parametros:
 *   m - metodo HTTP ("GET", "POST", ...)
 *   p - path exacto a matchear
 *   h - handler a invocar en caso de match
 */
static inline void add_route(const char *m, const char *p, handler_t h) {
    if (route_count < MAX_ROUTES) routes[route_count++] = (Route){m, p, h};
}

/* Azucar sintactica sobre add_route() para cada metodo HTTP soportado;
 * es lo que se usa en routes/index.h (get("/api", api), etc.). */
#define get(p, h)   add_route("GET", p, h)
#define post(p, h)  add_route("POST", p, h)
#define put(p, h)   add_route("PUT", p, h)
#define patch(p, h) add_route("PATCH", p, h)
#define del(p, h)   add_route("DELETE", p, h)

/*
 * res_json - fachada de respuesta JSON para los controladores
 *
 * Los controladores llaman a esto sin depender directamente de
 * utils/http/http.h; internamente delega en send_json().
 *
 * Parametros:
 *   r    - anillo io_uring del hilo actual
 *   f    - file descriptor del cliente
 *   json - texto JSON ya armado, listo para mandar como body
 */
static inline void res_json(struct io_uring *r, int f, const char *json) {
    send_json(r, f, json);
}

/* El prefijo /api esta reservado para rutas de backend: si no hubo match
 * exacto en la tabla, es un 404 real. Nunca debe caer en el fallback SPA
 * de static.h, o un typo en un endpoint devolveria el index.html de
 * Svelte con 200 en vez de 404. */
#define API_PREFIX "/api"

/*
 * path_is_api - indica si un path cae bajo el prefijo reservado /api
 *
 * Exige que lo que sigue a "/api" sea '/' o fin de cadena, para no
 * confundir "/apix" con el prefijo reservado.
 *
 * Parametros:
 *   p - path a evaluar
 *
 * Retorna:
 *   distinto de 0 si p empieza con "/api" seguido de '/' o NUL
 */
static inline int path_is_api(const char *p) {
    size_t n = sizeof(API_PREFIX) - 1;
    return strncmp(p, API_PREFIX, n) == 0 && (p[n] == '/' || p[n] == '\0');
}

/*
 * dispatch - punto de entrada del ruteo para cada request parseado
 *
 * Orden de resolucion:
 *   1. Busqueda lineal en routes[] por metodo+path exactos.
 *   2. Si no hay match y el path cae bajo /api: 404 directo (nunca SPA).
 *   3. Si no, se intenta servir como archivo estatico
 *      (try_serve_static, utils/http/static.h).
 *   4. Si nada de lo anterior aplico: 404.
 *
 * Parametros:
 *   r   - anillo io_uring del hilo actual
 *   fd  - file descriptor del cliente
 *   m   - metodo HTTP del request
 *   p   - path del request
 *   buf - buffer crudo del request (headers + body), se pasa tal cual
 *         al handler que matchee
 */
static inline void dispatch(struct io_uring *r, int fd, const char *m, const char *p, const char *buf) {
    for (int i = 0; i < route_count; i++) {
        if (strcmp(m, routes[i].method) == 0 && strcmp(p, routes[i].path) == 0) {
            routes[i].h(r, fd, m, buf);
            return;
        }
    }
    extern void error404(struct io_uring*, int, const char*, const char*);
    if (path_is_api(p)) return error404(r, fd, m, buf);
    if (try_serve_static(r, fd, m, p)) return;
    error404(r, fd, m, buf);
}

#endif
