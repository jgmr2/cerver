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
 *       1. Registrar los prepared statements de todos los modelos
 *          (register_models(), una sola vez, antes de crear hilos: el
 *          registro en config/db.c es un arreglo global, no __thread).
 *       2. Averiguar cuantos nucleos hay.
 *       3. Lanzar un hilo worker_loop() por nucleo.
 *       4. Instalar los manejadores de SIGTERM/SIGINT para apagado
 *          graceful (ver g_shutdown en utils/events.h): antes, un
 *          "docker stop" o un redeploy mataban el proceso en seco, a
 *          mitad de cualquier request en curso.
 *       5. Esperar (join) a que todos terminen. En operacion normal esto
 *          bloquea para siempre; ante SIGTERM/SIGINT, cada worker_loop
 *          nota la bandera, deja de aceptar conexiones nuevas, drena las
 *          que tiene en curso con un plazo maximo, y retorna — recien
 *          ahi los join()s se completan y el proceso termina.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include "core/server.h"
#include "models/registry.h"
#include "utils/events.h"
#include "config/db.h"
#include "utils/auth/jwt.h"
#include "utils/auth/password.h"
#include "utils/auth/login_limit.h"
#include "utils/net/conn_limit.h"

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
int g_db_startup_retry_attempts = 10;
int g_db_startup_retry_delay_seconds = 2;
long g_jwt_expires_seconds = 60L * 60L * 24L; /* 24 horas */
int g_pbkdf2_iterations = 100000;
int g_db_pool_size = 16;
int g_db_pending_queue_size = 8192;
/* Definiciones reales de los topes de utils/net/conn_limit.h. Default
 * pensado para una instancia chica (ver README.md, "AWS baratas"): 4096
 * conexiones totales por proceso alcanzan de sobra para trafico legitimo
 * y siguen siendo un tope real bien por debajo de NOFILE (65536, ver
 * docker-compose.yml); 100 por IP es generoso para un cliente real
 * (varias pestañas/keep-alive) pero corta un abuso concentrado antes de
 * que agote fds o memoria (CWE-770). Advertencia si se vuelve a poner un
 * proxy delante: ver el comentario de MAX_CONN_PER_IP en conn_limit.h. */
long g_max_global_connections = 4096;
int g_max_conn_per_ip = 100;
/* Definiciones reales de los topes de utils/auth/login_limit.h. Default:
 * 10 intentos fallidos por IP cada 60 segundos — bajo lo suficiente para
 * cortar fuerza bruta (cientos de miles de intentos/minuto sin esto) sin
 * bloquear a un usuario real que se equivoca de contraseña un par de
 * veces. */
long g_login_max_attempts = 10;
long g_login_window_seconds = 60;

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
    g_db_startup_retry_attempts = (int)read_long_env("DB_STARTUP_RETRY_ATTEMPTS", g_db_startup_retry_attempts, 1);
    g_db_startup_retry_delay_seconds = (int)read_long_env("DB_STARTUP_RETRY_DELAY_SECONDS", g_db_startup_retry_delay_seconds, 0);
    g_jwt_expires_seconds = read_long_env("JWT_EXPIRES_SECONDS", g_jwt_expires_seconds, 1);
    /* Piso de 1000: por debajo de eso PBKDF2 deja de ser un costo
     * significativo contra fuerza bruta offline si la DB se filtra. */
    g_pbkdf2_iterations = (int)read_long_env("PBKDF2_ITERATIONS", g_pbkdf2_iterations, 1000);
    g_db_pool_size = (int)read_long_env("DB_POOL_SIZE", g_db_pool_size, 1);
    g_db_pending_queue_size = (int)read_long_env("DB_PENDING_QUEUE_SIZE", g_db_pending_queue_size, 0);
    g_max_global_connections = read_long_env("MAX_CONNECTIONS", g_max_global_connections, 1);
    g_max_conn_per_ip = (int)read_long_env("MAX_CONN_PER_IP", g_max_conn_per_ip, 1);
    g_login_max_attempts = read_long_env("LOGIN_MAX_ATTEMPTS", g_login_max_attempts, 1);
    g_login_window_seconds = read_long_env("LOGIN_WINDOW_SECONDS", g_login_window_seconds, 1);
}

/*
 * require_jwt_secret - falla rapido al arrancar si JWT_SECRET no esta
 * definida
 *
 * utils/auth/jwt.c lee JWT_SECRET via getenv() en cada login/registro/
 * verificacion (no se cachea, ver el comentario en utils/auth/jwt.h);
 * sin este chequeo, faltar la variable no se nota hasta el primer
 * intento de login de un cliente real, que fallaria con 500 sin ninguna
 * pista de por que. Mismo criterio que DATABASE_URL en config/db.c:
 * mejor un error claro al arrancar que uno confuso en produccion.
 */
static void require_jwt_secret(void) {
    const char *secret = getenv("JWT_SECRET");
    if (!secret || !*secret) {
        fprintf(stderr, "FATAL: JWT_SECRET no encontrada\n");
        exit(1);
    }
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
    require_jwt_secret();
    read_tunables_from_env();

    /* Una sola vez, antes de que exista ningun hilo: puebla el registro
     * generico de prepared statements que cada init_db() (por hilo) leera. */
    register_models();

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
