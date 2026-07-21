/*
 * controllers/dbfiller_schema.h - esquemas .sql en <repo_root>/db/init/
 *
 * DESCRIPCION
 *     Puerto directo de la seccion "Esquema" del sidebar de la vieja GUI
 *     GTK4 (refresh_schema_list, on_schema_file_chosen, apply_schema_file
 *     en gui_main.c) a handlers HTTP.
 */
#ifndef DBFILLER_SCHEMA_CONTROLLER_H
#define DBFILLER_SCHEMA_CONTROLLER_H

#include <liburing.h>

void schema_files_handler(struct io_uring *r, int f, const char *m, const char *b);
void schema_upload_handler(struct io_uring *r, int f, const char *m, const char *b);
void schema_apply_handler(struct io_uring *r, int f, const char *m, const char *b);

#endif
