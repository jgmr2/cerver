/*
 * controllers/dbfiller_tables.h - listar tablas de la conexion activa +
 * generar CRUD
 *
 * DESCRIPCION
 *     Puerto directo de la pagina "Generar codigo" de la vieja GUI GTK4
 *     (on_connect_clicked poblando table_listbox, generate_one_checked/
 *     on_generate_clicked en gui_main.c) a handlers HTTP. La generacion
 *     en si no tiene logica propia aca -- todo pasa por
 *     dbfiller_generate_table() (../dbfiller_core/generate.h), la misma
 *     funcion que ya usan el CLI y usaba la GUI GTK4.
 */
#ifndef DBFILLER_TABLES_CONTROLLER_H
#define DBFILLER_TABLES_CONTROLLER_H

#include <liburing.h>

void tables_handler(struct io_uring *r, int f, const char *m, const char *b);
void generate_handler(struct io_uring *r, int f, const char *m, const char *b);

#endif
