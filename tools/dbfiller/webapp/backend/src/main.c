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
 *     entre hilos ni locks: cada uno escucha en el mismo puerto (g_port,
 *     ver core/server.h) gracias a SO_REUSEPORT (ver core/server.c), y el kernel reparte las
 *     conexiones entrantes entre ellos.
 *
 *     Este archivo solo se encarga de:
 *       1. Averiguar cuantos nucleos hay.
 *       2. Lanzar un hilo worker_loop() por nucleo.
 *       3. Instalar los manejadores de SIGTERM/SIGINT para apagado
 *          graceful (ver g_shutdown en utils/events.h): antes, un
 *          "docker stop" o un redeploy mataban el proceso en seco, a
 *          mitad de cualquier request en curso.
 *       4. Esperar (join) a que todos terminen. En operacion normal esto
 *          bloquea para siempre; ante SIGTERM/SIGINT, cada worker_loop
 *          nota la bandera, deja de aceptar conexiones nuevas, drena las
 *          que tiene en curso con un plazo maximo, y retorna — recien
 *          ahi los join()s se completan y el proceso termina.
 *
 *     Sin register_models()/models/registry.h a proposito: dbfiller-web
 *     no tiene modelos de negocio propios (no usa el pool ni los
 *     prepared statements del framework, ver el comentario grande en
 *     config/db.c, init_db()) -- su estado vive en
 *     controllers/dbfiller_state.c, no en config/db.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include "core/server.h"
#include "utils/events.h"
#include "config/db.h"

/* Definicion real de la bandera declarada extern en utils/events.h. */
volatile sig_atomic_t g_shutdown = 0;

/* Definicion real del puerto declarado extern en core/server.h. */
int g_port = 8080;

/* Definiciones reales de los tunables declarados extern en cada header
 * dueno del concepto (core/server.h, config/db.h, utils/auth/*.h); ver
 * read_long_env() mas abajo para como se resuelven desde el entorno. */
long g_shutdown_grace_seconds = 5;
long g_cache_refresh_seconds = 30;
int g_db_connect_timeout_seconds = 3;
long g_jwt_expires_seconds = 60L * 60L * 24L; /* 24 horas */
int g_pbkdf2_iterations = 100000;
int g_db_pool_size = 16;
int g_db_pending_queue_size = 8192;

/*
 * read_long_env - resuelve un entero desde una variable de entorno, con
 * piso minimo y valor por defecto
 *
 * Mismo criterio que read_port_from_env (mas abajo) generalizado: un
 * valor ausente, no numerico, o por debajo de 'min_value' deja el
 * default en vez de arrancar con una configuracion inesperada o fallar
 * mas adelante con un error dificil de rastrear hasta la variable de
 * entorno.
 *
 * Parametros:
 *   name       - nombre de la variable de entorno
 *   default_value - valor a usar si falta o es invalida
 *   min_value  - piso aceptado (inclusive); util para evitar, por
 *                ejemplo, un JWT_EXPIRES_SECONDS=0 o un
 *                PBKDF2_ITERATIONS tan bajo que el hash deje de ser
 *                seguro
 *
 * Retorna:
 *   el valor resuelto (de la variable de entorno o el default)
 */
static long read_long_env(const char *name, long default_value, long min_value) {
    const char *env = getenv(name);
    if (!env || !*env) return default_value;

    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || *end != '\0' || v < min_value) {
        fprintf(stderr, "%s invalida ('%s'), usando %ld por defecto\n", name, env, default_value);
        return default_value;
    }
    return v;
}

/*
 * read_port_from_env - resuelve el puerto de escucha desde la variable
 * de entorno PORT
 *
 * Se valida el rango de puerto TCP valido (1-65535); cualquier valor
 * ausente, no numerico o fuera de rango deja el default de 8080 en vez
 * de arrancar en un puerto inesperado o fallar el bind mas adelante con
 * un error dificil de rastrear hasta la variable de entorno.
 */
static void read_port_from_env(void) {
    const char *env = getenv("PORT");
    if (!env || !*env) return;

    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || *end != '\0' || v < 1 || v > 65535) {
        fprintf(stderr, "PORT invalida ('%s'), usando %d por defecto\n", env, g_port);
        return;
    }
    g_port = (int)v;
}

/*
 * read_tunables_from_env - resuelve todos los tunables operativos desde
 * el entorno
 *
 * Agrupa las lecturas de las variables que no necesitan validacion
 * especial (a diferencia de PORT y JWT_SECRET, que tienen su propia
 * funcion): timeouts en segundos, el costo de PBKDF2, y el tamano del
 * pool de conexiones a Postgres y su cola de espera (config/db.c aloca
 * la memoria de ambos en init_db(), usando los valores que dejamos acá
 * ya resueltos). Todas tienen un default razonable para desarrollo
 * local; se documentan en README.md.
 */
static void read_tunables_from_env(void) {
    g_shutdown_grace_seconds = read_long_env("SHUTDOWN_GRACE_SECONDS", g_shutdown_grace_seconds, 1);
    g_cache_refresh_seconds = read_long_env("CACHE_REFRESH_SECONDS", g_cache_refresh_seconds, 1);
    g_db_connect_timeout_seconds = (int)read_long_env("DB_CONNECT_TIMEOUT_SECONDS", g_db_connect_timeout_seconds, 1);
    g_jwt_expires_seconds = read_long_env("JWT_EXPIRES_SECONDS", g_jwt_expires_seconds, 1);
    /* Piso de 1000: por debajo de eso PBKDF2 deja de ser un costo
     * significativo contra fuerza bruta offline si la DB se filtra. */
    g_pbkdf2_iterations = (int)read_long_env("PBKDF2_ITERATIONS", g_pbkdf2_iterations, 1000);
    g_db_pool_size = (int)read_long_env("DB_POOL_SIZE", g_db_pool_size, 1);
    g_db_pending_queue_size = (int)read_long_env("DB_PENDING_QUEUE_SIZE", g_db_pending_queue_size, 0);
}

/*
 * on_shutdown_signal - manejador de SIGTERM/SIGINT
 *
 * Solo escribe la bandera: cualquier otra operacion (cerrar sockets,
 * liberar memoria, hacer I/O) no es async-signal-safe y podria
 * interrumpir al proceso en un estado inconsistente si el handler
 * corre en medio de esa misma operacion. Cada worker_loop es quien
 * reacciona a la bandera desde su propio bucle principal.
 */
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/*
 * main - registra los modelos, crea un hilo worker por nucleo y espera
 *
 * Retorna:
 *   0 en apagado normal (graceful o en la practica nunca, segun si llega
 *   una senal), 1 si falla la reserva de memoria o la creacion de algun
 *   hilo.
 */
int main() {
    /* Sin SA_RESTART a proposito: en operacion normal ningun worker esta
     * en un syscall que nos importe interrumpir salvo io_uring_enter (via
     * io_uring_submit_and_wait_timeout en core/server.c), que ya tiene su
     * propio timeout corto y no depende de EINTR para notar la bandera. */
    struct sigaction sa = {0};
    sa.sa_handler = on_shutdown_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    read_port_from_env();
    read_tunables_from_env();

    /* Numero de CPUs logicas visibles para este proceso. */
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1; /* sysconf puede fallar; con 1 hilo el server sigue funcionando */

    pthread_t t[n];

    printf("Arrancando servidor en puerto %d con %d hilos (Motor V8)...\n", g_port, n);

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
