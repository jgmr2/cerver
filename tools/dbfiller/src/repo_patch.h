/*
 * repo_patch.h - insercion/reemplazo idempotente de bloques marcados en
 * routes/index.h y models/registry.h
 *
 * DESCRIPCION
 *     Ambos archivos tienen (agregados una sola vez, a mano, como parte
 *     de habilitar dbfiller - ver tools/dbfiller/README.md) marcadores
 *     fijos de punto de insercion (comentarios "dbfiller:includes-point",
 *     "dbfiller:routes-point" en routes/index.h; "dbfiller:includes-point",
 *     "dbfiller:models-point" en models/registry.h).
 *
 *     Por tabla, este modulo inserta (o reemplaza si ya existe) un
 *     bloque delimitado por los comentarios "dbfiller:<tabla>:<tag>:begin"
 *     y "dbfiller:<tabla>:<tag>:end", inmediatamente antes del marcador
 *     de punto de insercion
 *     correspondiente, para que correr dbfiller de nuevo sobre la misma
 *     tabla actualice su bloque en vez de duplicarlo.
 */
#ifndef DBFILLER_REPO_PATCH_H
#define DBFILLER_REPO_PATCH_H

#include <stddef.h>

/*
 * repo_patch_apply - registra 'table_name' en routes/index.h (include +
 * rutas get/post_auth/put_auth/del_auth) y en models/registry.h
 * (include + llamada a <tabla>_register()).
 *
 * Parametros:
 *   repo_root  - raiz del repo cerver (donde viven routes/ y models/)
 *   table_name - nombre de la tabla/archivo generado
 *   has_update - si la tabla tiene columnas actualizables (ver codegen.h);
 *                si es 0, no se registra la ruta PUT (update_<tabla> ni
 *                siquiera existe en el controller generado)
 *
 * Retorna 0 en exito, -1 en error (err queda lleno; el caso mas comun
 * es que falte alguno de los marcadores de punto de insercion, que se
 * agregan una sola vez a mano).
 */
int repo_patch_apply(const char *repo_root, const char *table_name, int has_update, char *err, size_t err_len);

#endif
