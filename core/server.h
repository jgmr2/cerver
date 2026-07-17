/*
 * core/server.h - interfaz publica del hilo worker
 *
 * NOMBRE
 *     server.h - declara la funcion de entrada de cada hilo worker
 *
 * DESCRIPCION
 *     Header minimo a proposito: la implementacion completa (socket,
 *     io_uring, dispatch de eventos) vive en core/server.c. Este archivo
 *     solo expone lo que main.c necesita para lanzar los hilos.
 */
#ifndef CORE_SERVER_H
#define CORE_SERVER_H

/*
 * g_port - puerto TCP en el que escuchan todos los hilos worker
 *
 * Se lee una sola vez en main.c (variable de entorno PORT, o 8080 si no
 * esta definida) antes de crear ningun hilo, y de ahi en adelante es de
 * solo lectura: no hace falta que sea volatile ni __thread, porque el
 * happens-before de pthread_create garantiza que cada hilo worker vea el
 * valor ya asignado. La definicion real vive en main.c.
 */
extern int g_port;

/*
 * worker_loop - ciclo de vida completo de un hilo worker
 *
 * Crea su propio socket de escucha (SO_REUSEPORT en el puerto 8080),
 * su propio anillo io_uring y corre para siempre atendiendo eventos de
 * red y de base de datos. Ver la implementacion en core/server.c.
 *
 * Parametros:
 *   arg - puntero a un int reservado con malloc que identifica al hilo
 *         (0..N-1); worker_loop es responsable de liberarlo.
 *
 * Retorna:
 *   NULL siempre (la firma la exige pthread_create); en la practica no
 *   retorna porque el bucle interno es infinito.
 */
void *worker_loop(void *arg);

#endif
