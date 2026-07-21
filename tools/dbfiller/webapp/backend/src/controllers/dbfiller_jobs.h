/*
 * controllers/dbfiller_jobs.h - jobs en segundo plano con log y polling
 *
 * DESCRIPCION
 *     cerver no tiene soporte de streaming (SSE/WebSocket, ver el
 *     comentario grande en utils/http/http.h sobre send_res_bin armando
 *     la respuesta completa en memoria antes de un solo writev) -- los
 *     comandos largos que la vieja GUI GTK4 corria con
 *     run_streaming_command (docker build, docker compose up, `docker
 *     run ... psql`, `ab`) necesitan un modelo de jobs con polling en
 *     vez de push.
 *
 *     job_start() lanza un pthread separado (popen + fgets linea por
 *     linea, mismo patron que run_streaming_command de la GUI GTK4, ver
 *     tools/dbfiller/src/gui/gui_main.c en el historial de git) que
 *     appendea a un buffer propio del job en vez de a un GtkTextBuffer,
 *     y devuelve un id casi al instante sin bloquear el hilo HTTP que
 *     atendio la request. El cliente hace polling de
 *     GET /api/jobs/:id (job_poll_handler) hasta que "done" sea true --
 *     cada poll devuelve el log COMPLETO hasta el momento (no
 *     incremental por offset: el router no soporta query strings, ver
 *     el comentario en dbfiller_connection.c, y el volumen de estos
 *     logs -- unos KB, como mucho un par de cientos de KB para un `ab`
 *     grande -- no justifica la complejidad de trackear un offset por
 *     cliente).
 *
 *     Los jobs viven en un arreglo GLOBAL (no __thread, a diferencia de
 *     la tabla de rutas): cualquier request de polling puede caer en
 *     cualquier hilo worker, asi que tiene que ser visible desde
 *     cualquiera. MAX_JOBS slots, nunca se liberan/reusan dentro del
 *     tiempo de vida del proceso -- alcanza de sobra para una sesion de
 *     desarrollo local (32 builds/seeds/loadtests corridos sin
 *     reiniciar el contenedor es un caso extremo aceptado, no el uso
 *     normal).
 */
#ifndef DBFILLER_JOBS_H
#define DBFILLER_JOBS_H

#include <liburing.h>

#define MAX_JOBS 32

/*
 * job_start - lanza 'cmd' (via popen, "sh -c") en un pthread separado y
 * devuelve su id de job.
 *
 * Retorna:
 *   id >= 1 en exito; -1 si no queda ningun slot libre (MAX_JOBS
 *   agotados, ver el comentario de arriba).
 */
int job_start(const char *cmd);

/*
 * job_poll - snapshot del estado actual de un job
 *
 * Parametros:
 *   id             - id devuelto por job_start
 *   out_log        - se completa con una copia malloc'd del log
 *                     acumulado hasta el momento (el llamador hace
 *                     free()); NUL-terminado, puede ser cadena vacia
 *   out_done       - 1 si el proceso ya termino, 0 si sigue corriendo
 *   out_exit_code  - codigo de salida (solo valido si *out_done == 1)
 *
 * Retorna:
 *   0 si el id existe, -1 si no (id invalido o nunca asignado).
 */
int job_poll(int id, char **out_log, int *out_done, int *out_exit_code);

/* job_poll_handler - GET /api/jobs/:id -> {"done":bool,"exit_code":int,"log":"..."} */
void job_poll_handler(struct io_uring *r, int f, const char *m, const char *b);

#endif
