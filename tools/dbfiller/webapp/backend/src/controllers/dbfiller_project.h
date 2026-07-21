/*
 * controllers/dbfiller_project.h - raiz del repo destino, scaffolding de
 * proyecto nuevo, y editor de .env
 *
 * DESCRIPCION
 *     Puerto directo de las secciones "Proyecto" y "Variables de entorno
 *     (.env)" de la vieja GUI GTK4 (gui_main.c: on_choose_folder_clicked,
 *     on_scaffold_clicked, on_generate_env_defaults_clicked,
 *     on_save_env_clicked) a handlers HTTP.
 */
#ifndef DBFILLER_PROJECT_H
#define DBFILLER_PROJECT_H

#include <liburing.h>

void project_get_handler(struct io_uring *r, int f, const char *m, const char *b);
void project_set_handler(struct io_uring *r, int f, const char *m, const char *b);
void project_scaffold_handler(struct io_uring *r, int f, const char *m, const char *b);
void env_get_handler(struct io_uring *r, int f, const char *m, const char *b);
void env_save_handler(struct io_uring *r, int f, const char *m, const char *b);
void env_defaults_handler(struct io_uring *r, int f, const char *m, const char *b);

#endif
