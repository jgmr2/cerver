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
#include "../controllers/home.h"
/* dbfiller-web no usa el sistema de auth/pool del framework (ver el
 * comentario grande en config/db.c, init_db()) -- por eso no incluye
 * utils/auth/auth.h ni registra /api/auth/*: no hay usuarios ni JWT en
 * esta herramienta, es de un solo desarrollador local.
 *
 * Punto de insercion de los controllers propios de dbfiller-web: */
#include "../controllers/dbfiller_state.h"
#include "../controllers/dbfiller_jobs.h"
#include "../controllers/dbfiller_connection.h"
#include "../controllers/dbfiller_schema.h"
#include "../controllers/dbfiller_project.h"
/* TODO(dbfiller-web, en progreso): se van agregando a medida que cada
   controller queda implementado y probado con curl, ver el plan.
#include "../controllers/dbfiller_tables.h"
#include "../controllers/dbfiller_seed.h"
#include "../controllers/dbfiller_docker.h"
#include "../controllers/dbfiller_routes.h"
#include "../controllers/dbfiller_loadtest.h"
*/
/*
 * init_routes - llena la tabla de rutas del hilo actual
 *
 * Se llama una vez por hilo worker (ver core/server.c), porque la tabla
 * de rutas (routes[] en utils/http/router.h) es __thread: cada hilo
 * necesita su propia copia poblada antes de poder despachar requests.
 */
static inline void init_routes() {
    get("/healthz", healthz); /* controllers/home.h -- no toca el pool, sirve de healthcheck del contenedor */

    get("/api/jobs/:id", job_poll_handler);

    post("/api/testdb/up", testdb_up_handler);
    post("/api/testdb/down", testdb_down_handler);
    post("/api/connect", connect_handler);
    post("/api/databases", databases_handler);

    get("/api/schema/files", schema_files_handler);
    post("/api/schema/upload", schema_upload_handler);
    post("/api/schema/apply", schema_apply_handler);

    get("/api/project", project_get_handler);
    post("/api/project", project_set_handler);
    post("/api/project/scaffold", project_scaffold_handler);
    get("/api/env", env_get_handler);
    post("/api/env", env_save_handler);
    post("/api/env/defaults", env_defaults_handler);

    /* TODO(dbfiller-web, en progreso):
    get("/api/tables", tables_handler);
    post("/api/generate", generate_handler);
    post("/api/seed", seed_handler);
    post("/api/docker/build", docker_build_handler);
    get("/api/docker/status", docker_status_handler);
    get("/api/routes", routes_list_handler);
    post("/api/docker/up", docker_up_handler);
    post("/api/smoke-test", smoke_test_handler);
    post("/api/loadtest", loadtest_handler);
    */

    /* Raiz servida como contenido estatico plano (./public, el build de
     * Svelte de frontend/), con fallback a index.html para que el router
     * client-side de la SPA resuelva rutas propias. /api queda reservado
     * (ver path_is_api en router.h) y nunca cae en este fallback. */
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
 * necesite cachear algo en memoria agrega aca su propia linea (ver el
 * comentario de arriba, "dbfiller:routes-point", para el mismo
 * principio aplicado a rutas).
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 */
static inline void refresh_caches(struct io_uring *r) {
    (void)r; /* sin controladores con cache en memoria todavia */
}

#endif
