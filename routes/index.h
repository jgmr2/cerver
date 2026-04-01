#ifndef ROUTES_H
#define ROUTES_H
#include "../controllers/home.h"

typedef struct { char *path; void (*handler)(uv_stream_t *); } Route;

// Aquí la convención es ley: el path "/api" llama a "api"
Route router_map[] = {
    {"/",       home},
    {"/api",    api},
    {"/status", status}
};

void router(uv_stream_t *c, char *data) {
    // 1. Extraer el path (Zero-copy)
    char *p = strchr(data, ' ') + 1, *e = strchr(p, ' ');
    if (e) *e = '\0'; 

    // 2. Buscar en el mapa (Si el path es "/" lo cambiamos internamente a "home" o lo manejas directo)
    for (int i = 0; i < sizeof(router_map)/sizeof(Route); i++) {
        if (strcmp(p, router_map[i].path) == 0) {
            return router_map[i].handler(c);
        }
    }
    error404(c);
}
#endif
