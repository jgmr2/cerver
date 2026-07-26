/*
 * config/db.h - interfaz publica del pool de conexiones Postgres asincrono
 *
 * NOMBRE
 *     db.h - declara el tipo de conexion (db_t) y las funciones para
 *     lanzar consultas a Postgres sin bloquear el hilo worker
 *
 * DESCRIPCION
 *     Cada hilo worker (core/server.c) tiene su propio pool de
 *     g_db_pool_size conexiones libpq, todas en modo no bloqueante y
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

/*
 * g_db_pool_size - tamano del pool de conexiones a Postgres, por hilo
 * (reemplaza lo que antes era #define S 16)
 *
 * Variable de entorno DB_POOL_SIZE, default 16. Con N hilos worker, el
 * total de conexiones abiertas contra Postgres es N * g_db_pool_size —
 * tiene que quedar por debajo de max_connections de Postgres con margen
 * para otros clientes (psql, herramientas de administracion, etc.).
 *
 * pool[] (mas abajo) pasa de ser un arreglo estatico de tamano S a un
 * puntero reservado con calloc en init_db() usando este valor: por eso
 * es 'extern int' (solo lectura tras main(), como el resto de los
 * tunables) y no un #define — el tamano ya no se conoce en tiempo de
 * compilacion.
 */
extern int g_db_pool_size;

/*
 * g_db_pending_queue_size - capacidad de la cola de peticiones que
 * esperan una conexion libre (pending_q, config/db.c)
 *
 * Variable de entorno DB_PENDING_QUEUE_SIZE, default 8192 (reemplaza lo
 * que antes era #define DB_PENDING_Q).
 */
extern int g_db_pending_queue_size;

/*
 * g_db_connect_timeout_seconds - limite de tiempo para PQconnectdb
 * (init_db y reconnect_if_dead, config/db.c)
 *
 * Variable de entorno DB_CONNECT_TIMEOUT_SECONDS, default 3. Sin esto,
 * un Postgres inalcanzable puede colgar la conexion (inicial o de
 * reconexion) bastante mas de lo razonable.
 */
extern int g_db_connect_timeout_seconds;

/*
 * g_db_startup_retry_attempts / g_db_startup_retry_delay_seconds - cuantas
 * veces (y con que espera entre intento e intento) init_db() reintenta la
 * conexion inicial a Postgres antes de darse por vencido (config/db.c)
 *
 * Variables de entorno DB_STARTUP_RETRY_ATTEMPTS (default 10) y
 * DB_STARTUP_RETRY_DELAY_SECONDS (default 2). Sin esto, si el backend se
 * reinicia solo (restart: unless-stopped, docker-compose.yml) justo
 * cuando Postgres todavia no paso su healthcheck (p.ej. ambos
 * contenedores reiniciaron juntos), init_db() haria exit(1) de
 * inmediato — el proceso quedaria en crash-loop hasta que, por pura
 * casualidad de timing, la DB estuviera arriba en el instante exacto de
 * un reintento de Docker. El presupuesto por defecto (10 x 2s = 20s)
 * cubre el peor caso del healthcheck de `db` en docker-compose.yml
 * (interval 2s, retries 10). No reemplaza a reconnect_if_dead (esa es
 * para una conexion que ya estaba viva y se cayo en caliente); esto es
 * solo para el arranque del hilo, antes de que exista ningun pool.
 */
extern int g_db_startup_retry_attempts;
extern int g_db_startup_retry_delay_seconds;

/*
 * cb - callback invocado cuando una consulta asincrona termina
 *
 * Parametros:
 *   struct io_uring* - anillo del hilo, para responder o encadenar mas I/O
 *   int              - file descriptor del cliente que pidio la consulta
 *   PGresult*        - resultado de Postgres, o NULL si la consulta fallo
 *   void*            - el mismo puntero 'userdata' que se paso a
 *                       db_query_*_async al lanzar la consulta (NULL si
 *                       no se paso ninguno). Util para pasar contexto
 *                       calculado antes de la consulta (p.ej. la
 *                       contrasena en texto plano que hay que verificar
 *                       en el callback de login, ver utils/auth/auth.c)
 *                       que no viene en el PGresult. El callback es
 *                       responsable de liberarlo si lo reservo con
 *                       malloc/calloc — config/db.c solo lo transporta,
 *                       nunca es dueno de lo que apunta.
 *
 * Retorna (el callback):
 *   1 si se queda con la propiedad del PGresult (debe liberarlo el mismo
 *   con PQclear cuando termine de usarlo), 0 si config/db.c debe
 *   liberarlo inmediatamente despues de invocar el callback.
 */
typedef int(*cb)(struct io_uring*,int,PGresult*,void*);

/*
 * db_t - una conexion del pool y su estado
 *
 * Campos (empaquetados sin espacios a proposito, es una estructura de
 * uso interno frecuente):
 *   t        - tipo de evento (siempre EVENT_DB_POLL), para que server.c
 *              reconozca este puntero al llegar como dato de un CQE
 *   c        - conexion PGconn subyacente de libpq
 *   r        - anillo io_uring del hilo dueno de esta conexion
 *   f        - file descriptor del cliente que esta esperando el resultado
 *   s        - callback (cb) a invocar cuando la consulta termine
 *   b        - estado interno (DB_FREE / DB_READING / DB_FLUSHING, ver db.c)
 *   userdata - puntero opaco transportado hasta el callback (ver cb arriba)
 */
typedef struct{event_type_t t;PGconn*c;struct io_uring*r;int f;cb s;char b;void*userdata;}db_t;

/* Pool de conexiones del hilo actual: puntero (no arreglo de tamano
 * fijo), reservado con calloc(g_db_pool_size, ...) en init_db(). */
extern __thread db_t *pool;

/*
 * db_register_prepared - registra un prepared statement generico
 *
 * db.h/db.c no conocen ninguna tabla ni consulta especifica: cada modelo
 * (ver utils/auth/users.c, o cualquier models/<tabla>.c generado por
 * tools/dbfiller) declara aqui, con su propio nombre y su propio
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
 * init_db - crea las g_db_pool_size conexiones a Postgres del hilo actual
 *
 * Lee DATABASE_URL del entorno, conecta las conexiones, las pone en
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
 *   r        - anillo io_uring del hilo actual
 *   f        - file descriptor del cliente que espera el resultado
 *   q        - texto SQL a ejecutar
 *   s        - callback a invocar cuando la consulta termine (o falle)
 *   userdata - puntero opaco transportado hasta 's' (ver cb); NULL si no
 *              hace falta pasar nada
 */
void db_query_async(struct io_uring*r,int f,const char*q,cb s,void*userdata);

/*
 * db_query_prepared_async - lanza un prepared statement (formato texto)
 *
 * Equivale a db_query_prepared_fmt_async con result_format=0 (texto).
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   f        - file descriptor del cliente que espera el resultado
 *   stmt     - nombre del statement ya preparado en init_db()
 *   s        - callback a invocar cuando la consulta termine (o falle)
 *   userdata - puntero opaco transportado hasta 's' (ver cb); NULL si no
 *              hace falta pasar nada
 */
void db_query_prepared_async(struct io_uring *r, int f, const char *stmt, cb s, void *userdata);

/*
 * db_query_prepared_fmt_async - lanza un prepared statement eligiendo formato
 *
 * Igual que db_query_prepared_async pero permite pedir resultados en
 * formato binario (result_format=1), para un modelo que prefiera
 * decodificar filas sin pasar por texto en vez del formato 0 (texto)
 * que usa el resto del proyecto (incluido todo lo generado por
 * tools/dbfiller).
 *
 * Parametros:
 *   r             - anillo io_uring del hilo actual
 *   f             - file descriptor del cliente que espera el resultado
 *   stmt          - nombre del statement ya preparado en init_db()
 *   result_format - 0 = texto, 1 = binario (ver PQsendQueryPrepared)
 *   s             - callback a invocar cuando la consulta termine (o falle)
 *   userdata      - puntero opaco transportado hasta 's' (ver cb); NULL
 *                   si no hace falta pasar nada
 */
void db_query_prepared_fmt_async(struct io_uring *r, int f, const char *stmt, int result_format, cb s, void *userdata);

/*
 * db_query_prepared_params_async - lanza un prepared statement con
 * parametros reales (formato texto)
 *
 * A diferencia de db_query_prepared_async, esta variante no encola la
 * peticion si el pool esta agotado: la cola generica (pending_q, ver
 * db.c) no tiene forma de conservar un arreglo de parametros dinamicos
 * entre llamadas sin volver mas compleja esa estructura. Bajo pool
 * agotado se invoca el callback con res=NULL de inmediato, igual que si
 * la consulta hubiera fallado. Aceptable en la practica: pensada para
 * login/registro (utils/auth/auth.c), que no son el camino caliente de
 * este servidor a diferencia de las rutas de lectura de alto trafico
 * (p.ej. los "list"/"get" que genera tools/dbfiller para cada tabla).
 *
 * Los valores se mandan como texto plano (formato 0 de libpq); Postgres
 * los castea segun el tipo de columna de la posicion $N correspondiente
 * en el SQL del prepared statement. Nunca concatenar el valor al texto
 * SQL: siempre tiene que viajar por paramValues, es lo que evita SQL
 * injection en cualquier query que use datos de un cliente HTTP.
 *
 * Parametros:
 *   r             - anillo io_uring del hilo actual
 *   f             - file descriptor del cliente que espera el resultado
 *   stmt          - nombre del statement ya preparado en init_db()
 *   nParams       - cantidad de elementos en paramValues
 *   paramValues   - arreglo de strings NUL-terminados, uno por cada $N
 *                   del SQL en orden ($1, $2, ...)
 *   result_format - 0 = texto, 1 = binario
 *   s             - callback a invocar cuando la consulta termine (o falle)
 *   userdata      - puntero opaco transportado hasta 's' (ver cb); NULL
 *                   si no hace falta pasar nada
 */
void db_query_prepared_params_async(struct io_uring *r, int f, const char *stmt, int nParams, const char *const *paramValues, int result_format, cb s, void *userdata);
