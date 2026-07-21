/*
 * utils/http/router.c - definicion real del estado de parametros de ruta
 *
 * NOMBRE
 *     router.c - unica unidad de compilacion que define route_params /
 *     route_param_count
 *
 * DESCRIPCION
 *     router.h declara estos arreglos como extern (ver el comentario en
 *     ese header sobre por que no pueden ser static): este .c es donde
 *     vive la definicion real, para que cualquier handler, sin importar
 *     en que .c este definido, vea los mismos parametros capturados por
 *     el ultimo dispatch() en vez de una copia privada vacia.
 */
#include "router.h"

__thread route_param_t route_params[MAX_ROUTE_PARAMS];
__thread int route_param_count = 0;
