/*
 * controllers/dbfiller_state.h - estado global compartido de dbfiller-web
 *
 * DESCRIPCION
 *     Reemplaza a AppState de la vieja GUI GTK4, sin nada de GTK: la
 *     conexion Postgres "activa" (elegida en runtime via POST
 *     /api/connect, no una DATABASE_URL fija de negocio) y la raiz del
 *     repo destino. cerver es thread-per-core con tablas de rutas
 *     __thread (ver utils/http/router.h) -- cualquier request puede caer
 *     en cualquier hilo, asi que este estado tiene que ser GLOBAL (no
 *     __thread) y protegido por un mutex, a diferencia del resto del
 *     framework.
 *
 *     No hay sesiones ni multiples usuarios concurrentes contemplados a
 *     proposito: dbfiller-web es una herramienta de un solo desarrollador
 *     local, igual que ya era la GUI GTK4 que reemplaza.
 */
#ifndef DBFILLER_STATE_H
#define DBFILLER_STATE_H

#include <pthread.h>
#include "../dbfiller_core/introspect.h"

#define DBFILLER_REPO_ROOT_LEN 1024

#define DBFILLER_DB_URL_LEN 700

extern PGconn *g_conn;
extern pthread_mutex_t g_conn_mutex;
extern char g_repo_root[DBFILLER_REPO_ROOT_LEN];
/* g_database_url - la cadena de conexion que produjo g_conn (ver
   connect_handler). Guardada aparte porque PGconn* no expone de vuelta
   el conninfo original -- hace falta para acciones que no pasan por
   libpq sino por un subproceso aparte (docker run ... psql, ver
   dbfiller_schema.c/dbfiller_seed.c), que necesitan la URL como texto. */
extern char g_database_url[DBFILLER_DB_URL_LEN];

/* dbfiller_repo_root - raiz del repo actual, o "." si nunca se configuro
   (mismo fallback que ya usaba current_repo_root() en la GUI GTK). */
const char *dbfiller_repo_root(void);

#endif
