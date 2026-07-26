/*
 * utils/auth/login_limit.h - throttling de intentos fallidos de login por IP
 *
 * NOMBRE
 *     login_limit.h - ventana fija de intentos fallidos por IP, para
 *     mitigar fuerza bruta / credential stuffing contra
 *     POST /api/auth/login
 *
 * DESCRIPCION
 *     El limite de conexiones (utils/net/conn_limit.h) protege contra
 *     agotar recursos del proceso, pero no dice nada sobre CUANTAS VECES
 *     se puede probar una contraseña dentro de esas conexiones
 *     permitidas: con keep-alive, un solo origen puede probar cientos de
 *     miles de contraseñas por minuto sin que nada lo frene (el costo de
 *     PBKDF2 en utils/auth/password.c ralentiza cada intento individual,
 *     pero no reemplaza un limite explicito).
 *
 *     Implementa una ventana fija (fixed window) de intentos FALLIDOS
 *     por IP: si una IP acumula g_login_max_attempts fallos dentro de
 *     g_login_window_seconds, las siguientes verificaciones se rechazan
 *     con 429 sin tocar la base de datos ni calcular PBKDF2 — evita
 *     ademas que el ataque le pegue a Postgres o queme CPU en hashes que
 *     ya sabemos que van a fallar. La ventana se resetea sola al vencer,
 *     no hace falta un timer aparte (se resuelve de forma perezosa en el
 *     primer intento despues de vencida).
 *
 *     Deliberadamente por IP, no por username: un contador por username
 *     abre la puerta a que un atacante bloquee la cuenta de otra persona
 *     con solo mandar intentos fallidos usando SU username — un ataque
 *     de denegacion de servicio dirigido, mas facil de montar que el
 *     brute-force que se queria evitar en primer lugar.
 *
 *     Mismo patron que utils/net/conn_limit.h: contadores GLOBALES (no
 *     __thread — todos los hilos worker deben ver los mismos intentos
 *     fallidos de una IP, sin importar por cual hilo entro cada conexion
 *     via SO_REUSEPORT) implementados con atomics, sin locks, y con
 *     buckets hasheados en vez de una entrada exacta por IP (misma
 *     aproximacion tipo counting-sketch: dos IPs distintas pueden
 *     compartir cupo de intentos en el peor caso, aceptable para
 *     mitigar un ataque igual que en conn_limit.h).
 */
#ifndef UTILS_AUTH_LOGIN_LIMIT_H
#define UTILS_AUTH_LOGIN_LIMIT_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/* Potencia de 2, mismo criterio de tamaño que CONN_LIMIT_IP_BUCKETS
 * (utils/net/conn_limit.h): mantiene la probabilidad de colision baja
 * sin necesitar memoria dinamica. Es un arreglo de buckets INDEPENDIENTE
 * del de conn_limit.h a proposito: son dos limites de naturaleza
 * distinta (conexiones abiertas vs. intentos de login fallidos), cada
 * uno con su propia ventana de tiempo y su propio umbral. */
#define LOGIN_LIMIT_BUCKETS 8192u

/*
 * login_limit_bucket_t - estado de un bucket: cuantos fallos lleva la IP
 * (o las IPs) que cayeron ahi, y cuando arranco la ventana actual
 */
typedef struct {
    _Atomic int64_t window_start;
    _Atomic uint32_t fail_count;
} login_limit_bucket_t;

/* Definido en utils/auth/login_limit.c. */
extern login_limit_bucket_t g_login_limit_buckets[LOGIN_LIMIT_BUCKETS];

/*
 * g_login_max_attempts / g_login_window_seconds - topes configurables
 * por entorno (LOGIN_MAX_ATTEMPTS / LOGIN_WINDOW_SECONDS), resueltos una
 * sola vez en main.c (ver read_tunables_from_env). Solo lectura tras
 * main(): mismo criterio que g_port en core/server.h.
 */
extern long g_login_max_attempts;
extern long g_login_window_seconds;

/*
 * login_limit_hash_ip - reduce una IPv4 a un bucket de este modulo
 *
 * Misma mezcla de 32 bits que conn_limit_hash_ip (utils/net/conn_limit.h),
 * duplicada a proposito: son dos arreglos de buckets independientes, sin
 * ninguna relacion entre en que bucket cae una IP para conexiones y en
 * cual cae para intentos de login.
 *
 * Parametros:
 *   ip_be - direccion IPv4 en network byte order (ver conn_limit_get_ip)
 *
 * Retorna:
 *   indice de bucket en [0, LOGIN_LIMIT_BUCKETS)
 */
static inline uint32_t login_limit_hash_ip(uint32_t ip_be) {
    uint32_t h = ip_be;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h & (LOGIN_LIMIT_BUCKETS - 1);
}

/*
 * login_limit_allowed - indica si esta IP todavia puede intentar un login
 *
 * Si la ventana del bucket ya vencio, la resetea antes de decidir (CAS
 * sobre window_start: si dos hilos la ven vencida al mismo tiempo, solo
 * uno gana el reset — el otro simplemente relee un fail_count ya en 0,
 * en vez de resetearlo dos veces y potencialmente pisar un fallo que
 * otro hilo concurrente acababa de sumar). No consume un intento por si
 * sola: eso lo hace login_limit_record_failure, solo despues de un
 * fallo real, para no penalizar logins correctos.
 *
 * Parametros:
 *   ip_be - IP de origen (ver conn_limit_get_ip, utils/net/conn_limit.h)
 *
 * Retorna:
 *   distinto de 0 si todavia hay cupo en la ventana actual, 0 si ya se
 *   alcanzo g_login_max_attempts (el caller debe responder 429 sin
 *   llamar a Users_find_by_username_async ni calcular PBKDF2).
 */
static inline int login_limit_allowed(uint32_t ip_be) {
    login_limit_bucket_t *b = &g_login_limit_buckets[login_limit_hash_ip(ip_be)];
    int64_t now = (int64_t)time(NULL);
    int64_t start = atomic_load_explicit(&b->window_start, memory_order_relaxed);

    if (now - start >= g_login_window_seconds) {
        int64_t expected = start;
        if (atomic_compare_exchange_strong_explicit(&b->window_start, &expected, now,
                                                      memory_order_relaxed, memory_order_relaxed)) {
            atomic_store_explicit(&b->fail_count, 0, memory_order_relaxed);
        }
    }

    return atomic_load_explicit(&b->fail_count, memory_order_relaxed) < (uint32_t)g_login_max_attempts;
}

/*
 * login_limit_record_failure - contabiliza un intento de login fallido
 *
 * Llamar solo cuando el login efectivamente fallo (usuario inexistente o
 * password incorrecta, ver on_user_found_for_login en utils/auth/auth.c)
 * — un login exitoso no debe sumar al contador.
 *
 * Parametros:
 *   ip_be - IP de origen del intento fallido
 */
static inline void login_limit_record_failure(uint32_t ip_be) {
    login_limit_bucket_t *b = &g_login_limit_buckets[login_limit_hash_ip(ip_be)];
    atomic_fetch_add_explicit(&b->fail_count, 1, memory_order_relaxed);
}

/*
 * login_limit_record_success - limpia el contador de una IP tras un
 * login correcto
 *
 * Puramente para no penalizar a un usuario legitimo que tipeo mal la
 * contraseña un par de veces antes de acertar: sin esto, sus intentos
 * fallidos previos igual iban a expirar solos con la ventana, esto solo
 * lo adelanta. No es necesario para que el limite funcione.
 *
 * Parametros:
 *   ip_be - IP de origen del login exitoso
 */
static inline void login_limit_record_success(uint32_t ip_be) {
    login_limit_bucket_t *b = &g_login_limit_buckets[login_limit_hash_ip(ip_be)];
    atomic_store_explicit(&b->fail_count, 0, memory_order_relaxed);
}

#endif
