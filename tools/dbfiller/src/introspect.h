/*
 * introspect.h - lectura de esquema de Postgres via libpq
 *
 * DESCRIPCION
 *     dbfiller es un CLI de una sola corrida (a diferencia del server
 *     principal, que nunca bloquea el hilo worker con I/O de red -
 *     ver CONCURRENCY.md en la raiz del repo, recuperable con
 *     `git show e9b774d:CONCURRENCY.md`): aca alcanza con libpq en modo
 *     sincronico, PQconnectdb/PQexecParams comunes.
 */
#ifndef DBFILLER_INTROSPECT_H
#define DBFILLER_INTROSPECT_H

#include <postgresql/libpq-fe.h>
#include "schema.h"

#define MAX_TABLES 512
#define MAX_ERROR_LEN 512

/*
 * pg_connect - conecta a Postgres
 *
 * Parametros:
 *   conninfo - cadena de conexion libpq (URI o key=value); si es NULL,
 *              usa la variable de entorno DATABASE_URL
 *   err/err_len - buffer de error
 *
 * Retorna: conexion abierta, o NULL si fallo (err queda lleno)
 */
PGconn *pg_connect(const char *conninfo, char *err, size_t err_len);

/*
 * pg_list_tables - tablas base del schema 'public', en orden alfabetico
 *
 * Retorna: cantidad de tablas encontradas (>= 0), o -1 en error
 */
int pg_list_tables(PGconn *conn, char out_names[][MAX_NAME_LEN], int max_tables, char *err, size_t err_len);

/*
 * pg_list_databases - bases de datos no-template del servidor, en orden
 * alfabetico, usando una conexion ya abierta.
 *
 * A diferencia de MySQL, Postgres no necesita una conexion especial "sin
 * base seleccionada": pg_database es un catalogo compartido a nivel de
 * servidor, visible desde cualquier conexion sin importar a que base
 * especifica este conectada (ver pg_conninfo_with_dbname para conectarse
 * a una base "cualquiera" solo para poder listar las demas).
 *
 * Retorna: cantidad de bases encontradas (>= 0), o -1 en error
 */
int pg_list_databases(PGconn *conn, char out_names[][MAX_NAME_LEN], int max_dbs, char *err, size_t err_len);

/*
 * pg_conninfo_with_dbname - toma un conninfo/URI cualquiera (lo que haya
 * en el campo DATABASE_URL de la GUI, o --database-url del CLI) y arma
 * uno nuevo con el dbname reemplazado por new_dbname, preservando
 * host/puerto/usuario/contrasena. Usa PQconninfoParse (libpq), asi que
 * acepta tanto el formato URI (postgresql://user:pass@host:port/db) como
 * el de keyword=value.
 *
 * Pensado para dos usos: (1) conectarse a una base cualquiera (ej.
 * "postgres") solo para poder listar las demas via pg_list_databases, y
 * (2) una vez el usuario elige una base de esa lista, reconstruir la
 * conninfo real apuntando ahi.
 *
 * Retorna 0 en exito (out queda lleno), -1 si el conninfo de entrada no
 * se pudo parsear (err queda lleno).
 */
int pg_conninfo_with_dbname(const char *conninfo, const char *new_dbname, char *out, size_t out_size, char *err, size_t err_len);

/*
 * pg_load_table - trae columnas y PK de una tabla del schema 'public'
 *
 * out->pk_index queda en -1 si la tabla no tiene PK o tiene una PK
 * compuesta (mas de una columna) - el llamador decide si eso es motivo
 * para saltear la tabla.
 *
 * Retorna: 0 si la tabla existe y se pudo leer (aunque no tenga PK usable),
 * -1 en error (incluida "la tabla no existe": 0 columnas encontradas)
 */
int pg_load_table(PGconn *conn, const char *table_name, PgTable *out, char *err, size_t err_len);

#endif
