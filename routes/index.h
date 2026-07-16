/*
 * routes/index.h - tabla de rutas del servidor
 *
 * NOMBRE
 *     index.h - registra cada endpoint HTTP y el mount estatico raiz
 *
 * DESCRIPCION
 *     Punto unico donde se listan todas las rutas: cada endpoint nuevo
 *     agrega una linea con get()/post()/etc. (ver utils/http/router.h) y
 *     un #include del controlador que lo implementa.
 */
#ifndef ROUTES_INDEX_H
#define ROUTES_INDEX_H

#include "../utils/http/router.h"
#include "../controllers/sakila.h"
#include "../controllers/home.h"
/*
 * init_routes - llena la tabla de rutas del hilo actual
 *
 * Se llama una vez por hilo worker (ver core/server.c), porque la tabla
 * de rutas (routes[] en utils/http/router.h) es __thread: cada hilo
 * necesita su propia copia poblada antes de poder despachar requests.
 */
static inline void init_routes() {
    get("/api", api);
    get("/api/sakila/films/top", get_sakila_top_films);
    get("/api/sakila/actors/top", get_sakila_top_actors);


    /* Raiz servida por el build de Svelte, con fallback a index.html para
     * que su router client-side resuelva rutas como /about, /users/42,
     * etc. /api queda reservado (ver path_is_api en router.h) y nunca
     * cae en este fallback. */
    mount_static("/", "./public", 1);
}

#endif
