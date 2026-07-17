/*
 * utils/events.h - tipos de evento del anillo io_uring
 *
 * NOMBRE
 *     events.h - enum de tipos de evento y contexto del primer read
 *
 * DESCRIPCION
 *     Todo puntero de contexto que viaja como "user data" de un SQE/CQE
 *     de io_uring (ver core/server.c) empieza con un campo
 *     event_type_t, para que el bucle principal pueda reinterpretar el
 *     puntero segun el tipo sin necesitar una tabla aparte.
 */
#ifndef UTILS_EVENTS_H
#define UTILS_EVENTS_H

#include <signal.h>

/*
 * g_shutdown - bandera de apagado graceful, escrita solo por el manejador
 * de SIGTERM/SIGINT (ver main.c)
 *
 * sig_atomic_t es el unico tipo cuya lectura/escritura esta garantizada
 * como atomica dentro de un manejador de senales en C estandar; volatile
 * evita que el compilador la cachee en un registro y nunca vea el cambio.
 *
 * No es __thread a proposito: es la MISMA bandera para los 8 hilos
 * worker, para que un solo SIGTERM les avise a todos. Se declara extern
 * aqui (no static) por la misma razon que conn_keep_alive en
 * utils/http/http.h: este header se incluye desde varias unidades de
 * compilacion (core/server.c, controllers/sakila.c via http.h, ...) y
 * todas necesitan ver la misma variable, no copias privadas por archivo.
 * La definicion real vive en main.c.
 */
extern volatile sig_atomic_t g_shutdown;

/*
 * event_type_t - identifica que clase de operacion completo un CQE
 *
 *   EVENT_ACCEPT            - una conexion nueva fue aceptada
 *   EVENT_HTTP_READ_INITIAL - se leyeron los bytes iniciales de un request
 *   EVENT_HTTP_HELPER       - continuacion de escritura de respuesta o de
 *                              I/O de archivo (ver utils/http/http.h)
 *   EVENT_DB_POLL           - continuacion de una consulta a Postgres
 *                              (ver config/db.c)
 *   EVENT_CACHE_TICK        - vencio el timer periodico de refresco de
 *                              caches en memoria (ver refresh_caches en
 *                              routes/index.h y core/server.c)
 */
typedef enum {
    EVENT_ACCEPT,
    EVENT_HTTP_READ_INITIAL,
    EVENT_HTTP_HELPER,
    EVENT_DB_POLL,
    EVENT_CACHE_TICK
} event_type_t;

/*
 * initial_read_ctx_t - contexto del primer (o siguiente, en keep-alive)
 * read de una conexion
 *
 * Campos:
 *   type      - siempre EVENT_HTTP_READ_INITIAL
 *   client_fd - file descriptor de la conexion que se esta leyendo
 *   buf       - buffer donde io_uring escribe los bytes leidos; se pasa
 *               tal cual a phr_parse_request (core/server.c)
 */
typedef struct {
    event_type_t type;
    int client_fd;
    char buf[4096];
} initial_read_ctx_t;

/*
 * cache_tick_ctx_t - contexto del timer periodico de refresco de caches
 *
 * Sin datos propios mas alla del tipo: a diferencia de
 * initial_read_ctx_t, una sola instancia por hilo alcanza y se reutiliza
 * en cada re-armado del timer (mismo patron que accept_ctx_t en
 * core/server.c), en vez de pedir una nueva con calloc en cada tick.
 */
typedef struct {
    event_type_t type;
} cache_tick_ctx_t;

#endif
