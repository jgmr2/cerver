/*
 * utils/auth/password.h - hashing y verificacion de contrasenas
 *
 * NOMBRE
 *     password.h - PBKDF2-HMAC-SHA256 sobre OpenSSL (ya enlazado, ver
 *     LIBS en el Makefile), sin dependencias nuevas
 *
 * DESCRIPCION
 *     Cada llamada a password_hash() genera una sal aleatoria distinta
 *     (RAND_bytes) y la codifica junto al hash resultante en un solo
 *     string de texto ("salt_hex:hash_hex"), listo para guardar en la
 *     columna password_hash de la tabla users (ver
 *     db/init/01_auth_schema.sql). password_verify() separa esa cadena,
 *     recalcula el hash con la misma sal y compara en tiempo constante
 *     (CRYPTO_memcmp) para no filtrar por temporizacion cuanto de la
 *     contrasena coincidio.
 *
 *     Ambas funciones son sincronas (CPU pura, sin I/O): no hay
 *     violacion de la regla de "nada bloquea al hilo worker" (ver
 *     CONCURRENCY.md) al llamarlas desde un handler.
 */
#ifndef UTILS_AUTH_PASSWORD_H
#define UTILS_AUTH_PASSWORD_H

#include <stddef.h>

/* Tamano minimo recomendado para el buffer de salida de password_hash:
 * cantidad de iteraciones en texto + 16 bytes de sal + 32 bytes de hash,
 * cada uno en hex (x2) mas los separadores ':' y el NUL, con margen. */
#define PASSWORD_HASH_BUF_SIZE 160

/*
 * g_pbkdf2_iterations - costo de PBKDF2 (utils/auth/password.c)
 *
 * Variable de entorno PBKDF2_ITERATIONS, default 100000. Solo lectura
 * tras main(): un cambio en caliente no invalida hashes ya guardados
 * (password_hash() graba las iteraciones usadas dentro del propio hash
 * almacenado, y password_verify() las lee de ahi, no de esta variable
 * — asi se puede subir el costo con el tiempo sin romper el login de
 * usuarios existentes).
 */
extern int g_pbkdf2_iterations;

/*
 * password_hash - genera sal aleatoria y hashea una contrasena
 *
 * Parametros:
 *   password - contrasena en texto plano, NUL-terminada
 *   out      - buffer de salida (PASSWORD_HASH_BUF_SIZE o mas)
 *   out_sz   - tamano de 'out'
 *
 * Retorna:
 *   1 en exito (out queda con "salt_hex:hash_hex", NUL-terminado), 0 si
 *   'out_sz' es insuficiente o si RAND_bytes/PBKDF2 fallan.
 */
int password_hash(const char *password, char *out, size_t out_sz);

/*
 * password_verify - verifica una contrasena contra un hash almacenado
 *
 * Parametros:
 *   password - contrasena en texto plano a verificar
 *   stored   - valor tal cual viene de la columna password_hash
 *              ("salt_hex:hash_hex", ver password_hash)
 *
 * Retorna:
 *   1 si la contrasena coincide, 0 si no coincide o si 'stored' esta
 *   mal formado.
 */
int password_verify(const char *password, const char *stored);

#endif
