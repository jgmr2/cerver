/*
 * config/db.h - interfaz publica del pool de conexiones Postgres asincrono
 *
 * NOMBRE
 *     db.h - declara el tipo de conexion (db_t) y las funciones para
 *     lanzar consultas a Postgres sin bloquear el hilo worker
 *
 * DESCRIPCION
 *     Cada hilo worker (core/server.c) tiene su propio pool de S
 *     conexiones libpq, todas en modo no bloqueante (PQsetnonblocking) y
 *     multiplexadas sobre el mismo anillo io_uring del hilo via
 *     io_uring_prep_poll_add sobre el file descriptor del socket de
 *     libpq. No hay hilos dedicados a la base de datos ni bloqueo alguno:
 *     el kernel avisa cuando el socket esta listo para leer o escribir y
 *     el CQE correspondiente se procesa en config/db.c (handle_db_cqe).
 */
#pragma once
#include <postgresql/libpq-fe.h>
#include <liburing.h>
#include "../utils/events.h"

/* Tamano del pool de conexiones por hilo. */
#define S 16

/*
 * cb - callback invocado cuando una consulta asincrona termina
 *
 * Parametros:
 *   struct io_uring* - anillo del hilo, para responder o encadenar mas I/O
 *   int              - file descriptor del cliente que pidio la consulta
 *   PGresult*        - resultado de Postgres, o NULL si la consulta fallo
 *
 * Retorna (el callback):
 *   1 si se queda con la propiedad del PGresult (debe liberarlo el mismo
 *   con PQclear cuando termine de usarlo), 0 si config/db.c debe
 *   liberarlo inmediatamente despues de invocar el callback.
 */
typedef int(*cb)(struct io_uring*,int,PGresult*);

/*
 * db_t - una conexion del pool y su estado
 *
 * Campos (empaquetados sin espacios a proposito, es una estructura de
 * uso interno frecuente):
 *   t - tipo de evento (siempre EVENT_DB_POLL), para que server.c
 *       reconozca este puntero al llegar como dato de un CQE
 *   c - conexion PGconn subyacente de libpq
 *   r - anillo io_uring del hilo dueno de esta conexion
 *   f - file descriptor del cliente que esta esperando el resultado
 *   s - callback (cb) a invocar cuando la consulta termine
 *   b - estado interno (DB_FREE / DB_READING / DB_FLUSHING, ver db.c)
 *   p - reservado / no usado por la logica actual
 */
typedef struct{event_type_t t;PGconn*c;struct io_uring*r;int f;cb s;char b;int p;}db_t;

/* Pool de conexiones del hilo actual; se llena en init_db(). */
extern __thread db_t pool[S];

/* Cupo maximo de prepared statements que puede registrar el conjunto de
 * modelos via db_register_prepared(). */
#define DB_MAX_PREPARED 64

/*
 * db_register_prepared - registra un prepared statement generico
 *
 * db.h/db.c no conocen ninguna tabla ni consulta especifica: cada modelo
 * (ver models/sakila.c) declara aqui, con su propio nombre y su propio
 * SQL, los statements que necesita. init_db() prepara cada entrada
 * registrada en todas las conexiones del pool de cada hilo.
 *
 * Debe llamarse una sola vez, antes de crear los hilos worker (ver
 * register_models() en models/registry.h, invocada desde main.c): el
 * registro es un arreglo global, no __thread, por lo que poblarlo desde
 * varios hilos a la vez duplicaria entradas.
 *
 * Parametros:
 *   name - nombre con el que luego se invoca (PQsendQueryPrepared /
 *          db_query_prepared_async)
 *   sql  - texto SQL del statement
 */
void db_register_prepared(const char *name, const char *sql);

/*
 * init_db - crea las S conexiones a Postgres del hilo actual
 *
 * Lee DATABASE_URL del entorno, conecta las S conexiones, las pone en
 * modo no bloqueante y prepara en cada una todos los statements que se
 * hayan registrado con db_register_prepared() antes de llamar a esta
 * funcion. Sale del proceso (exit(1)) si falta la variable de entorno o
 * si cualquier paso de conexion/preparado falla: sin base de datos el
 * servidor no tiene forma util de seguir operando.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual, guardado en cada db_t del pool
 */
void init_db(struct io_uring*r);

/*
 * handle_db_cqe - continua una conexion tras un evento de poll de io_uring
 *
 * Se llama desde core/server.c cuando llega un CQE con tipo
 * EVENT_DB_POLL. Segun el estado interno de la conexion (DB_FLUSHING o
 * DB_READING) sigue enviando la consulta, sigue leyendo el resultado, o
 * si ya esta completo invoca el callback registrado y recicla o libera
 * la conexion.
 *
 * Parametros:
 *   x - conexion del pool asociada a este evento
 *   e - CQE recibido; e->res < 0 indica error de poll
 */
void handle_db_cqe(db_t*x,struct io_uring_cqe*e);

/*
 * db_query_async - lanza una consulta SQL de texto plano sin bloquear
 *
 * Si no hay conexiones libres, la consulta se encola (ver
 * enqueue_pending en db.c) y se atiende en cuanto se libere alguna; si la
 * cola tambien esta llena, se invoca el callback con res=NULL de
 * inmediato.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente que espera el resultado
 *   q - texto SQL a ejecutar
 *   s - callback a invocar cuando la consulta termine (o falle)
 */
void db_query_async(struct io_uring*r,int f,const char*q,cb s);

/*
 * db_query_prepared_async - lanza un prepared statement (formato texto)
 *
 * Equivale a db_query_prepared_fmt_async con result_format=0 (texto).
 *
 * Parametros:
 *   r    - anillo io_uring del hilo actual
 *   f    - file descriptor del cliente que espera el resultado
 *   stmt - nombre del statement ya preparado en init_db()
 *   s    - callback a invocar cuando la consulta termine (o falle)
 */
void db_query_prepared_async(struct io_uring *r, int f, const char *stmt, cb s);

/*
 * db_query_prepared_fmt_async - lanza un prepared statement eligiendo formato
 *
 * Igual que db_query_prepared_async pero permite pedir resultados en
 * formato binario (result_format=1), que es lo que usa
 * controllers/sakila.c para decodificar filas sin pasar por texto.
 *
 * Parametros:
 *   r             - anillo io_uring del hilo actual
 *   f             - file descriptor del cliente que espera el resultado
 *   stmt          - nombre del statement ya preparado en init_db()
 *   result_format - 0 = texto, 1 = binario (ver PQsendQueryPrepared)
 *   s             - callback a invocar cuando la consulta termine (o falle)
 */
void db_query_prepared_fmt_async(struct io_uring *r, int f, const char *stmt, int result_format, cb s);
