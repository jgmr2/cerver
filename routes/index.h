#ifndef ROUTES_H
#define ROUTES_H

#include <liburing.h>
#include <string.h>

#include "../controllers/home.h"

typedef struct { 
    const char *p; 
    void (*h)(struct io_uring *, int); 
} Route;

// Puedes agregar tus demás rutas aquí
Route rs[] = {
    {"/api", api},
    {"/status", status},
    {"/", home}
};

// El router ahora recibe la ruta limpia directamente (ej. "/api")
void router(struct io_uring *ring, int client_fd, const char *path) {
    // Buscamos coincidencia exacta en nuestro arreglo de rutas
    for (size_t i = 0; i < sizeof(rs)/sizeof(Route); i++) {
        if (!strcmp(path, rs[i].p)) {
            return rs[i].h(ring, client_fd);
        }
    }
    
    // Si ninguna coincide, lanzamos 404
    error404(ring, client_fd);
}

#endif