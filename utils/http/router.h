#ifndef ROUTER_H
#define ROUTER_H

#include <liburing.h>
#include <string.h>
#include "http.h" // Router es el dueño de la utilidad HTTP

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

#define get(p, h)  add_route("GET", p, h)
#define post(p, h) add_route("POST", p, h)

// Fachada de respuesta: El controlador llama a esto, pero esto usa http.h internamente
static inline void res_json(struct io_uring *r, int f, const char *json) {
    send_json(r, f, json);
}

static inline void dispatch(struct io_uring *r, int fd, const char *m, const char *p, const char *buf) {
    for (int i = 0; i < route_count; i++) {
        if (strcmp(m, routes[i].method) == 0 && strcmp(p, routes[i].path) == 0) {
            routes[i].h(r, fd, m, buf);
            return;
        }
    }
    extern void error404(struct io_uring*, int, const char*, const char*);
    error404(r, fd, m, buf);
}

#endif