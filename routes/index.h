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
#include "../controllers/auth.h"
/*
 * init_routes - llena la tabla de rutas del hilo actual
 *
 * Se llama una vez por hilo worker (ver core/server.c), porque la tabla
 * de rutas (routes[] en utils/http/router.h) es __thread: cada hilo
 * necesita su propia copia poblada antes de poder despachar requests.
 */
static inline void init_routes() {
    get("/api", api);
    get("/healthz", healthz);
    get("/api/echo/:msg", echo); /* ejemplo de ruta con parametro, ver controllers/home.h */
    get("/api/sakila/films/top", get_sakila_top_films);
    get("/api/sakila/actors/top", get_sakila_top_actors);

    post("/api/auth/register", register_user);
    post("/api/auth/login", login_user);
    /* Ejemplo de ruta protegida: exige "Authorization: Bearer <token>"
     * valido (ver dispatch en utils/http/router.h) antes de invocar el
     * handler. Plantilla para cualquier endpoint que necesite saber
     * quien es el usuario autenticado. */
    get_auth("/api/me", me);

    /* Documentacion interactiva de la API (Swagger UI, vendorizado en
     * docs-ui/), servida como contenido estatico plano — no lleva
     * fallback SPA (spa_fallback=0): un asset de la doc que falte tiene
     * que dar 404 real, no caer en el index.html del frontend.
     *
     * Se registra ANTES del mount "/": try_serve_static() (utils/http/static.h)
     * recorre static_mounts[] en orden y el mount "/" matchea CUALQUIER
     * path (prefijo de un solo caracter), asi que si se registrara
     * primero, "/docs/..." nunca llegaria a este mount. */
    mount_static("/docs", "./docs-ui", 0);

    /* Raiz servida como contenido estatico plano (./public), con fallback
     * a index.html para que un router client-side (si el frontend que se
     * monte aca tiene uno) resuelva rutas como /about, /users/42, etc.
     * /api queda reservado (ver path_is_api en router.h) y nunca cae en
     * este fallback. */
    mount_static("/", "./public", 1);
}

/*
 * refresh_caches - dispara el refresco de todos los caches en memoria
 * que hayan registrado los controladores
 *
 * Mismo principio que init_routes(): core/server.c no conoce que caches
 * existen ni que controlador es dueno de cada uno, solo llama a esta
 * funcion (una vez al arrancar el hilo y despues periodicamente cada
 * CACHE_REFRESH_SECONDS, ver core/server.c). Cada controlador que
 * necesite cachear algo en memoria agrega aca su propia linea, como
 * sakila_refresh_actors_cache (ver controllers/sakila.h).
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 */
static inline void refresh_caches(struct io_uring *r) {
    sakila_refresh_actors_cache(r);
}

#endif
