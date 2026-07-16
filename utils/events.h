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

/*
 * event_type_t - identifica que clase de operacion completo un CQE
 *
 *   EVENT_ACCEPT            - una conexion nueva fue aceptada
 *   EVENT_HTTP_READ_INITIAL - se leyeron los bytes iniciales de un request
 *   EVENT_HTTP_HELPER       - continuacion de escritura de respuesta o de
 *                              I/O de archivo (ver utils/http/http.h)
 *   EVENT_DB_POLL           - continuacion de una consulta a Postgres
 *                              (ver config/db.c)
 */
typedef enum {
    EVENT_ACCEPT,
    EVENT_HTTP_READ_INITIAL,
    EVENT_HTTP_HELPER,
    EVENT_DB_POLL
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

#endif
