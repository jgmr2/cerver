/*
 * utils/auth/jwt.h - creacion y verificacion de JWT (HS256 unicamente)
 *
 * NOMBRE
 *     jwt.h - JSON Web Tokens firmados con HMAC-SHA256 sobre OpenSSL
 *
 * DESCRIPCION
 *     Implementacion deliberadamente acotada a un solo algoritmo
 *     (HS256, secreto compartido) en vez de soportar la lista completa
 *     de algoritmos que permite el estandar JWT. Esa flexibilidad es
 *     justo la que habilita el ataque de "confusion de algoritmo" mas
 *     comun en implementaciones de JWT (el cliente declara "alg":"HS256"
 *     en un token cuya firma en realidad se genero con la clave publica
 *     RS256 del servidor, tratandola como si fuera el secreto
 *     compartido). Este modulo nunca lee el campo "alg" del token para
 *     decidir como verificar: siempre asume HS256 y recalcula la firma
 *     esperada con el secreto propio, asi que ese vector queda cerrado
 *     por diseño, no por validacion.
 *
 *     jwt_create/jwt_verify_and_store son sincronas (CPU pura: HMAC,
 *     base64url, parseo de un JSON chico) — no violan la regla de nada
 *     bloquea al hilo worker (ver CONCURRENCY.md).
 *
 *     El secreto (JWT_SECRET) se lee de la variable de entorno en cada
 *     llamada via getenv(), igual que DATABASE_URL en config/db.c: no
 *     cambia en caliente, así que cachearlo no aporta nada y evita
 *     sumar otro estado __thread mas para mantener sincronizado.
 */
#ifndef UTILS_AUTH_JWT_H
#define UTILS_AUTH_JWT_H

#include <stddef.h>

#define JWT_SUB_MAX 64
#define JWT_USERNAME_MAX 64
/* Tamano de buffer que alcanza para cualquier token que este modulo
 * pueda emitir (sub/username acotados + campos fijos de timestamp). */
#define JWT_TOKEN_BUF_SIZE 512

/*
 * g_jwt_expires_seconds - vida de un JWT recien emitido (utils/auth/auth.c)
 *
 * Variable de entorno JWT_EXPIRES_SECONDS, default 24hs. Solo lectura
 * tras main(): un cambio en caliente no afecta tokens ya emitidos (cada
 * uno lleva su propio "exp" grabado, ver jwt_create), solo a los que se
 * emitan de ahi en mas.
 */
extern long g_jwt_expires_seconds;

/*
 * jwt_create - arma y firma un JWT HS256
 *
 * Parametros:
 *   secret             - secreto compartido (JWT_SECRET)
 *   sub                - claim "sub" (tipicamente el id de usuario, como texto)
 *   username            - claim "username"
 *   expires_in_seconds - tiempo de vida del token desde ahora
 *   out                - buffer de salida (JWT_TOKEN_BUF_SIZE o mas)
 *   out_sz             - tamano de 'out'
 *
 * Retorna:
 *   1 en exito (out queda con el token completo, NUL-terminado), 0 si
 *   algun paso de codificacion/firma fallo o el buffer de salida no
 *   alcanzo.
 */
int jwt_create(const char *secret, const char *sub, const char *username, long expires_in_seconds, char *out, size_t out_sz);

/*
 * jwt_verify_and_store - verifica un JWT HS256 y, si es valido, guarda
 * sus claims para consultarlos con jwt_claim()
 *
 * Verifica en este orden: estructura (header.payload.signature, sin
 * puntos de mas), firma (HMAC recalculado, comparado en tiempo
 * constante), y vencimiento (claim "exp" contra la hora actual). No
 * hay tolerancia de reloj (leeway): un token vencido hace un segundo ya
 * es invalido.
 *
 * Parametros:
 *   secret - secreto compartido (JWT_SECRET)
 *   token  - token tal cual llego en el header Authorization
 *
 * Retorna:
 *   1 si el token es valido y no vencio (los claims quedan disponibles
 *   via jwt_claim() para el resto del request), 0 en cualquier otro
 *   caso (firma invalida, estructura invalida, vencido).
 */
int jwt_verify_and_store(const char *secret, const char *token);

/*
 * jwt_claim - consulta un claim del ultimo jwt_verify_and_store exitoso
 *
 * Solo tiene sentido llamarlo despues de un jwt_verify_and_store que
 * devolvio 1, dentro del mismo request (el estado es __thread y se pisa
 * en cada verificacion, no sobrevive entre requests).
 *
 * Parametros:
 *   name - "sub" o "username" (los unicos claims que este modulo guarda)
 *
 * Retorna:
 *   el valor del claim (cadena NUL-terminada), o NULL si no hay una
 *   verificacion exitosa vigente o el nombre no es reconocido.
 */
const char *jwt_claim(const char *name);

#endif
