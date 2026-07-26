/*
 * utils/net/conn_limit.h - limite de conexiones concurrentes (global y por IP)
 *
 * NOMBRE
 *     conn_limit.h - admision/liberacion de conexiones contra un tope
 *     global y un tope por IP de origen
 *
 * DESCRIPCION
 *     Sin este modulo, nada en el servidor limita cuantas conexiones TCP
 *     puede tener abiertas un mismo cliente a la vez: el unico timeout
 *     existente (_rearm_read, utils/http/http.h) protege de una conexion
 *     lenta individual (slowloris), no de miles de conexiones abiertas en
 *     paralelo por el mismo origen agotando el pool de file descriptors o
 *     la memoria del proceso (CWE-770, "Allocation of Resources Without
 *     Limits or Throttling"). Esto solo importa cuando el backend recibe
 *     la IP real del cliente en el socket (hoy: siempre, no hay proxy
 *     delante). Si en el futuro se vuelve a poner nginx u otro proxy
 *     delante, TODAS las conexiones van a llegar con la IP del proxy, y
 *     el tope por IP pasa a comportarse como un tope global disfrazado:
 *     en ese escenario hay que subir MAX_CONN_PER_IP a algo mayor que
 *     MAX_CONNECTIONS (o excluir la IP del proxy) para no auto-limitarse.
 *
 *     Los contadores son GLOBALES al proceso (no __thread): con
 *     SO_REUSEPORT el kernel reparte las conexiones de una misma IP entre
 *     los N hilos worker sin ningun criterio de afinidad (ver
 *     worker_loop, core/server.c), asi que un tope por IP solo tiene
 *     sentido si lo ven todos los hilos a la vez. Se implementan con
 *     atomics (stdatomic.h) en vez de un mutex: el proyecto entero evita
 *     locks entre hilos, y un fetch_add/fetch_sub no bloqueante alcanza
 *     para este caso (no hace falta ninguna operacion compuesta
 *     atomica-respecto-de-otra-variable).
 *
 *     El limite por IP usa un arreglo fijo de contadores indexados por
 *     hash(ip) % CONN_LIMIT_IP_BUCKETS en vez de una tabla hash real con
 *     una entrada por IP: es una aproximacion tipo "counting sketch" —
 *     dos IPs distintas que caen en el mismo bucket comparten el cupo, lo
 *     cual es aceptable para mitigar un ataque (el objetivo es negar un
 *     abuso concentrado, no contar con precision perfecta por IP) y evita
 *     necesitar memoria dinamica o un lock para resolver colisiones.
 *
 *     El fd de cada conexion aceptada recuerda en que bucket quedo
 *     contado (conn_ip_bucket[fd], mismo patron __thread+extern que
 *     conn_keep_alive en utils/http/http.h) para poder liberar
 *     exactamente ese cupo cuando la conexion se cierra, sin importar
 *     cuantos requests keep-alive haya atendido en el medio.
 */
#ifndef UTILS_NET_CONN_LIMIT_H
#define UTILS_NET_CONN_LIMIT_H

#include <stdatomic.h>
#include <stdint.h>

/* Cantidad de buckets del arreglo de contadores por IP. Potencia de 2
 * para que el modulo sea un AND bit a bit. 8192 buckets mantiene la
 * probabilidad de colision baja para el volumen de conexiones simultaneas
 * que un solo hilo puede sostener en la practica. */
#define CONN_LIMIT_IP_BUCKETS 8192u

/* Mismo tope que MAX_TRACKED_FD en utils/http/http.h: conn_ip_bucket[]
 * necesita el mismo rango de indices que conn_keep_alive[]. No se
 * incluye http.h aca a proposito: conn_limit.h no depende de HTTP, se
 * aplica al aceptar la conexion TCP, antes de leer o parsear ningun
 * byte. */
#define CONN_LIMIT_MAX_FD 65536

/*
 * g_max_global_connections / g_max_conn_per_ip - topes configurables por
 * entorno (MAX_CONNECTIONS / MAX_CONN_PER_IP), resueltos una sola vez en
 * main.c (ver read_tunables_from_env). Solo lectura tras main(): mismo
 * criterio que g_port en core/server.h.
 */
extern long g_max_global_connections;
extern int g_max_conn_per_ip;

/*
 * g_global_conn_count / g_ip_bucket_count - contadores reales,
 * compartidos por TODOS los hilos (no __thread a proposito, ver
 * DESCRIPCION arriba). Definidos en utils/net/conn_limit.c.
 */
extern atomic_uint g_global_conn_count;
extern atomic_uint g_ip_bucket_count[CONN_LIMIT_IP_BUCKETS];

/*
 * conn_ip_bucket[fd] - bucket+1 donde quedo contado el fd, o 0 si el fd
 * no esta bajo control de este modulo (conexion rechazada, o ya
 * liberada).
 *
 * __thread porque cada hilo solo toca los fd que el mismo acepto: la
 * tabla de descriptores es unica por proceso, pero como cada accept()
 * viene de un socket distinto por hilo (SO_REUSEPORT), el kernel nunca
 * le devuelve a dos hilos el mismo numero de fd al mismo tiempo — mismo
 * razonamiento que conn_keep_alive (utils/http/http.h). La definicion
 * real vive en utils/net/conn_limit.c.
 */
extern __thread uint32_t conn_ip_bucket[CONN_LIMIT_MAX_FD];

/*
 * conn_remote_ip[fd] - IP de origen (network byte order) de la conexion
 * admitida en ese fd, o 0 si el fd no esta bajo control de este modulo.
 *
 * Se llena en el mismo punto que conn_ip_bucket (conn_limit_try_accept),
 * reutilizando la unica oportunidad que existe de ver el sockaddr real
 * del cliente (core/server.c, EVENT_ACCEPT). Sin esto, ningun handler
 * (p.ej. login_user en utils/auth/auth.c) tendria forma de saber de que
 * IP vino el request que esta procesando — dispatch() solo les pasa
 * (ring, fd, metodo, body). __thread por el mismo motivo que
 * conn_ip_bucket. La definicion real vive en utils/net/conn_limit.c.
 */
extern __thread uint32_t conn_remote_ip[CONN_LIMIT_MAX_FD];

/*
 * conn_limit_hash_ip - reduce una IPv4 (network byte order) a un bucket
 *
 * Mezcla de 32 bits (variante de MurmurHash3 finalizer), suficiente para
 * distribuir IPs reales de forma pareja entre los CONN_LIMIT_IP_BUCKETS
 * buckets. Una IP elegida a proposito para colisionar con otra no rompe
 * nada: en el peor caso, dos atacantes comparten cupo.
 *
 * Parametros:
 *   ip_be - direccion IPv4 tal cual llega en sockaddr_in.sin_addr.s_addr
 *
 * Retorna:
 *   indice de bucket en [0, CONN_LIMIT_IP_BUCKETS)
 */
static inline uint32_t conn_limit_hash_ip(uint32_t ip_be) {
    uint32_t h = ip_be;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h & (CONN_LIMIT_IP_BUCKETS - 1);
}

/*
 * conn_limit_try_accept - intenta admitir una conexion nueva bajo los
 * topes global y por IP
 *
 * Incrementa ambos contadores de forma atomica; si cualquiera de los dos
 * topes se supera, revierte el incremento antes de devolver 0.
 *
 * Parametros:
 *   fd    - file descriptor recien aceptado (debe estar en
 *           [0, CONN_LIMIT_MAX_FD) para poder trackearse; fuera de rango
 *           se rechaza directamente)
 *   ip_be - IP de origen (sockaddr_in.sin_addr.s_addr)
 *
 * Retorna:
 *   1 si la conexion fue admitida (fd queda registrado y DEBE liberarse
 *   con conn_limit_release al cerrarse), 0 si se supero algun tope (el
 *   caller debe cerrar el fd directamente, sin llamar a
 *   conn_limit_release: nunca quedo registrado).
 */
static inline int conn_limit_try_accept(int fd, uint32_t ip_be) {
    if (fd < 0 || fd >= CONN_LIMIT_MAX_FD) return 0;

    unsigned prev_global = atomic_fetch_add_explicit(&g_global_conn_count, 1, memory_order_relaxed);
    if ((long)prev_global + 1 > g_max_global_connections) {
        atomic_fetch_sub_explicit(&g_global_conn_count, 1, memory_order_relaxed);
        return 0;
    }

    uint32_t bucket = conn_limit_hash_ip(ip_be);
    unsigned prev_ip = atomic_fetch_add_explicit(&g_ip_bucket_count[bucket], 1, memory_order_relaxed);
    if ((int)prev_ip + 1 > g_max_conn_per_ip) {
        atomic_fetch_sub_explicit(&g_ip_bucket_count[bucket], 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&g_global_conn_count, 1, memory_order_relaxed);
        return 0;
    }

    conn_ip_bucket[fd] = bucket + 1;
    conn_remote_ip[fd] = ip_be;
    return 1;
}

/*
 * conn_limit_get_ip - IP de origen de una conexion ya admitida
 *
 * Parametros:
 *   fd - file descriptor de la conexion (tipicamente el mismo fd que
 *        recibe un handler de ruta, ver utils/http/router.h)
 *
 * Retorna:
 *   la IP (network byte order) con la que se admitio ese fd, o 0 si el
 *   fd esta fuera de rango o nunca paso por conn_limit_try_accept.
 */
static inline uint32_t conn_limit_get_ip(int fd) {
    if (fd < 0 || fd >= CONN_LIMIT_MAX_FD) return 0;
    return conn_remote_ip[fd];
}

/*
 * conn_limit_release - libera el cupo ocupado por un fd previamente
 * admitido
 *
 * Idempotente a proposito: llamarla dos veces sobre el mismo fd (o sobre
 * un fd que nunca paso por conn_limit_try_accept) no hace nada la
 * segunda vez, gracias al centinela conn_ip_bucket[fd] == 0. Esto
 * importa porque el servidor tiene varios puntos de salida distintos
 * para una conexion (EOF en el read inicial, respuesta con
 * "Connection: close", error a mitad del pipeline de archivos
 * estaticos, ...) y es mas seguro que cada uno pueda llamar a esto sin
 * coordinarse entre si que arriesgarse a un decremento de mas, que
 * underflowearia el contador atomico (unsigned) y rompería el limite
 * para siempre.
 *
 * Parametros:
 *   fd - file descriptor que se esta cerrando
 */
static inline void conn_limit_release(int fd) {
    if (fd < 0 || fd >= CONN_LIMIT_MAX_FD) return;
    uint32_t b = conn_ip_bucket[fd];
    if (b == 0) return;
    conn_ip_bucket[fd] = 0;
    atomic_fetch_sub_explicit(&g_ip_bucket_count[b - 1], 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_global_conn_count, 1, memory_order_relaxed);
}

#endif
