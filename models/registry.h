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

static inline void register_models(void) {
    sakila_register();
}

#endif
