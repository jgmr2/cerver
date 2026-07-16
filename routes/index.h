#ifndef ROUTES_INDEX_H
#define ROUTES_INDEX_H

#include "../utils/http/router.h"
// Importamos los controladores que vayamos creando
#include "../controllers/sakila.h"
#include "../controllers/home.h"

static inline void init_routes() {
    get("/api", api);
    get("/api/sakila/films/top", get_sakila_top_films);
    get("/api/sakila/actors/top", get_sakila_top_actors);

    // Raiz servida por el build de Svelte, con fallback a index.html para que
    // su router client-side resuelva rutas como /about, /users/42, etc.
    // /api queda reservado (ver path_is_api en router.h) y nunca cae aqui.
    mount_static("/", "./public", 1);
}

#endif