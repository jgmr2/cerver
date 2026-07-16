/*
 * main.c - punto de entrada del proceso
 *
 * NOMBRE
 *     main - arranca un hilo worker por cada nucleo de CPU disponible
 *
 * DESCRIPCION
 *     El servidor usa un modelo "thread-per-core": cada hilo corre su
 *     propio anillo io_uring, su propio pool de conexiones a la base de
 *     datos (ver config/db.c) y su propia tabla de rutas (ver
 *     routes/index.h), todo declarado __thread. No hay estado compartido
 *     entre hilos ni locks: cada uno escucha en el mismo puerto 8080
 *     gracias a SO_REUSEPORT (ver core/server.c), y el kernel reparte las
 *     conexiones entrantes entre ellos.
 *
 *     Este archivo solo se encarga de:
 *       1. Registrar los prepared statements de todos los modelos
 *          (register_models(), una sola vez, antes de crear hilos: el
 *          registro en config/db.c es un arreglo global, no __thread).
 *       2. Averiguar cuantos nucleos hay.
 *       3. Lanzar un hilo worker_loop() por nucleo.
 *       4. Esperar (join) a que todos terminen, lo cual en la practica
 *          nunca ocurre salvo error fatal, manteniendo el proceso vivo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "core/server.h"
#include "models/registry.h"

/*
 * main - registra los modelos, crea un hilo worker por nucleo y espera
 *
 * Retorna:
 *   0 en apagado normal (en la practica no se alcanza), 1 si falla la
 *   reserva de memoria o la creacion de algun hilo.
 */
int main() {
    /* Una sola vez, antes de que exista ningun hilo: puebla el registro
     * generico de prepared statements que cada init_db() (por hilo) leera. */
    register_models();

    /* Numero de CPUs logicas visibles para este proceso. */
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1; /* sysconf puede fallar; con 1 hilo el server sigue funcionando */

    pthread_t t[n];

    printf("Arrancando servidor en puerto 8080 con %d hilos (Motor V8)...\n", n);

    for (int i = 0; i < n; i++) {
        /* El id se pasa por puntero porque pthread_create solo admite un
         * void* como argumento; cada hilo es responsable de liberarlo
         * (ver worker_loop en core/server.c). */
        int *id = malloc(sizeof(int));
        if (!id) {
            perror("Error al reservar memoria para hilos");
            return 1;
        }
        *id = i;

        if (pthread_create(&t[i], NULL, worker_loop, id) != 0) {
            perror("Error al crear hilo");
            free(id);
            return 1;
        }
    }

    /* worker_loop() corre un bucle infinito, asi que este join bloquea
     * para siempre en operacion normal: es lo que mantiene main() (y por
     * lo tanto el proceso) vivo mientras los workers atienden conexiones. */
    for (int i = 0; i < n; i++) {
        pthread_join(t[i], NULL);
    }

    printf("\nServidor apagado.\n");
    return 0;
}
