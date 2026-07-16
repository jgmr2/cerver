/*
 * utils/http/http.c - definicion real del estado de keep-alive por hilo
 *
 * NOMBRE
 *     http.c - unica unidad de compilacion que define conn_keep_alive
 *
 * DESCRIPCION
 *     http.h declara conn_keep_alive como extern (ver el comentario en
 *     ese header sobre por que no puede ser static): este .c es donde
 *     vive la definicion real, para que todos los archivos .c que
 *     incluyen http.h (core/server.c, controllers/sakila.c, etc.) vean
 *     el mismo arreglo por hilo en vez de copias privadas.
 */
#include "http.h"

__thread unsigned char conn_keep_alive[MAX_TRACKED_FD];
