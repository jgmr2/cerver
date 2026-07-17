/*
 * codegen.h - genera controllers/<tabla>.c/.h y models/<tabla>.c/.h a
 * partir de un PgTable ya introspeccionado (ver introspect.h)
 */
#ifndef DBFILLER_CODEGEN_H
#define DBFILLER_CODEGEN_H

#include <stddef.h>
#include "schema.h"

#define DBFILLER_GENERATED_MARKER "/* Generado por dbfiller -- ver tools/dbfiller. No editar a mano si vas a re-generar. */"

/*
 * codegen_write_table - genera y escribe los 4 archivos de 'table' bajo
 * repo_root (controllers/<t>.c/.h, models/<t>.c/.h).
 *
 * El llamador debe validar antes que table->pk_index >= 0 (una tabla
 * sin PK usable no es responsabilidad de este modulo).
 *
 * Un archivo destino que ya existe y cuya primera linea no es
 * DBFILLER_GENERATED_MARKER (es decir, es codigo escrito a mano) no se
 * sobreescribe salvo force != 0 -- en ese caso se devuelve error.
 *
 * Parametros de salida:
 *   has_update_out - se completa con 1 si la tabla tiene al menos una
 *                    columna actualizable (aparte de la PK); el
 *                    llamador (repo_patch) lo necesita para saber si
 *                    debe registrar la ruta PUT.
 *
 * Retorna 0 en exito, -1 en error (err queda lleno).
 */
int codegen_write_table(const PgTable *table, const char *repo_root, int force, int *has_update_out, char *err, size_t err_len);

#endif
