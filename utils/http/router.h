#ifndef ROUTER_H
#define ROUTER_H

#include <liburing.h>
#include <string.h>
#include "http.h" // Router es el dueño de la utilidad HTTP
#include "static.h" // mount_static / try_serve_static

// El controlador ahora es más simple: (ring, fd, metodo, cuerpo_peticion)
typedef void (*handler_t)(struct io_uring*, int, const char*, const char*);

typedef struct {
    const char *method;
    const char *path;
    handler_t h;
} Route;

#define MAX_ROUTES 128
static __thread Route routes[MAX_ROUTES];
static __thread int route_count = 0;

static inline void add_route(const char *m, const char *p, handler_t h) {
    if (route_count < MAX_ROUTES) routes[route_count++] = (Route){m, p, h};
}

#define get(p, h)   add_route("GET", p, h)
#define post(p, h)  add_route("POST", p, h)
#define put(p, h)   add_route("PUT", p, h)
#define patch(p, h) add_route("PATCH", p, h)
#define del(p, h)   add_route("DELETE", p, h)

// Fachada de respuesta: El controlador llama a esto, pero esto usa http.h internamente
static inline void res_json(struct io_uring *r, int f, const char *json) {
    send_json(r, f, json);
}

// El prefijo /api esta reservado para rutas de backend: si no hubo match exacto
// arriba, es un 404 real. Nunca debe caer en el fallback SPA de static.h, o un
// typo en un endpoint devolveria el index.html de Svelte con 200 en vez de 404.
#define API_PREFIX "/api"

static inline int path_is_api(const char *p) {
    size_t n = sizeof(API_PREFIX) - 1;
    return strncmp(p, API_PREFIX, n) == 0 && (p[n] == '/' || p[n] == '\0');
}

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