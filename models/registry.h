/*
 * models/registry.h - punto unico de registro de todos los modelos
 *
 * NOMBRE
 *     registry.h - agrega las funciones *_register() de cada modelo
 *
 * DESCRIPCION
 *     Mismo patron que routes/index.h con init_routes(): cada modelo
 *     nuevo agrega una linea aqui con su propia funcion de registro
 *     (p.ej. sakila_register()). register_models() se llama una sola
 *     vez desde main.c, antes de crear los hilos worker, porque el
 *     registro de prepared statements en config/db.c es un arreglo
 *     global (no __thread): si cada hilo llamara a esto por su cuenta,
 *     las entradas se duplicarian.
 */
#ifndef MODELS_REGISTRY_H
#define MODELS_REGISTRY_H

#include "../models/sakila.h"
#include "../models/users.h"
/* Punto de insercion de tools/dbfiller: cada tabla generada agrega su
 * propio #include "../models/<tabla>.h" justo antes de esta linea (ver
 * tools/dbfiller/src/repo_patch.c). No borrar este comentario. */
/* dbfiller:includes-point */

static inline void register_models(void) {
    sakila_register();
    users_register();
    /* Punto de insercion de tools/dbfiller: cada tabla generada agrega
     * aca su propia llamada a <tabla>_register() (ver
     * tools/dbfiller/src/repo_patch.c). No borrar este comentario. */
    /* dbfiller:models-point */
}

#endif
