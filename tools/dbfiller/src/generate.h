/*
 * generate.h - orquestacion de "generar una tabla", compartida por el
 * CLI (main.c) y la GUI (gui/gui_main.c)
 *
 * DESCRIPCION
 *     Ninguno de los dos front-ends reimplementa el flujo completo
 *     (cargar esquema -> validar PK -> generar archivos -> registrar en
 *     routes/index.h y models/registry.h): ambos llaman a
 *     dbfiller_generate_table() y solo deciden como mostrar el
 *     resultado (stdout/stderr en el CLI, un panel de texto en la GUI).
 */
#ifndef DBFILLER_GENERATE_H
#define DBFILLER_GENERATE_H

#include "introspect.h"

typedef struct {
    char table_name[MAX_NAME_LEN];
    int ok;         /* 1 = generado y registrado, 0 = error (ver message) */
    int has_update; /* solo valido si ok=1: si la tabla tiene columnas actualizables */
    char message[MAX_ERROR_LEN]; /* motivo del error si ok=0; vacio si ok=1 */
} GenerateResult;

/*
 * dbfiller_generate_table - genera controllers/models CRUD para una
 * tabla y la registra en routes/index.h y models/registry.h
 *
 * Combina pg_load_table (introspect.h), codegen_write_table (codegen.h)
 * y repo_patch_apply (repo_patch.h). No imprime nada: el llamador arma
 * el mensaje que quiera mostrar a partir de *out.
 *
 * Parametros:
 *   conn       - conexion ya abierta (ver pg_connect, introspect.h)
 *   table_name - tabla del schema 'public' a generar
 *   repo_root  - raiz del repo cerver (donde viven controllers/, models/, routes/)
 *   force      - sobreescribe archivos existentes aunque no tengan la
 *                marca de dbfiller (ver codegen.h)
 *   out        - resultado; out->table_name siempre se completa, el
 *                resto segun out->ok
 */
void dbfiller_generate_table(PGconn *conn, const char *table_name, const char *repo_root, int force, GenerateResult *out);

#endif
