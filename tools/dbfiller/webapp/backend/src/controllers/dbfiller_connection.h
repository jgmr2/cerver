/*
 * controllers/dbfiller_connection.h - Postgres de prueba desechable +
 * conexion activa + listado de bases del servidor
 *
 * DESCRIPCION
 *     Puerto directo de la seccion "Conexion" del sidebar de la vieja
 *     GUI GTK4 (on_testdb_start/stop_clicked, on_connect_clicked,
 *     on_list_databases_clicked en gui_main.c, historial de git) a
 *     handlers HTTP. Sin query strings en ningun lado (el router no los
 *     soporta, ver utils/http/router.h): todo lo que antes eran campos
 *     de formulario ahora viaja en el body JSON de un POST, incluso para
 *     lo que semanticamente es una lectura (POST /api/databases) --
 *     una DATABASE_URL no entra segura en un segmento de path (lleva
 *     ':'/'/'/'@').
 */
#ifndef DBFILLER_CONNECTION_H
#define DBFILLER_CONNECTION_H

#include <liburing.h>

void testdb_up_handler(struct io_uring *r, int f, const char *m, const char *b);
void testdb_down_handler(struct io_uring *r, int f, const char *m, const char *b);
void connect_handler(struct io_uring *r, int f, const char *m, const char *b);
void databases_handler(struct io_uring *r, int f, const char *m, const char *b);

#endif
