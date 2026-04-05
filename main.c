#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "core/server.h"

int main() {
    // Obtenemos el número de núcleos
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1; // Fallback por si sysconf falla

    pthread_t t[n];
    
    printf("🚀 Arrancando servidor en puerto 8080 con %d hilos (Motor V8)...\n", n);

    for (int i = 0; i < n; i++) {
        // Reservamos memoria para el ID del hilo
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

    // Esperamos a que los hilos terminen (esto mantiene el proceso vivo)
    for (int i = 0; i < n; i++) {
        pthread_join(t[i], NULL);
    }
    
    printf("\n🛑 Servidor apagado.\n");
    return 0;
}