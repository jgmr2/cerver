/*
 * controllers/auth.h - handlers de registro/login (JWT propio)
 *
 * NOMBRE
 *     auth.h - POST /api/auth/register y POST /api/auth/login
 *
 * DESCRIPCION
 *     Ambos endpoints leen {"username":"...","password":"..."} del body
 *     JSON del request (utils/json.h) y, en exito, responden
 *     {"token":"<jwt>"} listo para mandar en el header
 *     "Authorization: Bearer <token>" de requests subsiguientes a rutas
 *     protegidas (get_auth/post_auth/etc., ver utils/http/router.h).
 */
#ifndef CONTROLLERS_AUTH_H
#define CONTROLLERS_AUTH_H

#include <liburing.h>

/*
 * register_user - handler de POST /api/auth/register
 *
 * Crea un usuario nuevo con la contrasena hasheada (nunca en texto
 * plano, ver utils/auth/password.h) y responde con un JWT ya firmado
 * (auto-login tras registrarse). 400 si falta username/password o el
 * username no pasa la validacion (ver controllers/auth.c); 409 si el
 * username ya existe.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso (la ruta ya filtro por POST)
 *   b - buffer crudo del request (headers + body)
 */
void register_user(struct io_uring *r, int f, const char *m, const char *b);

/*
 * login_user - handler de POST /api/auth/login
 *
 * Busca el usuario, verifica la contrasena, y responde con un JWT
 * nuevo. 401 tanto si el usuario no existe como si la contrasena no
 * coincide — la misma respuesta en ambos casos, para no dejarle saber a
 * quien intenta entrar si el username que probo existe o no.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso
 *   b - buffer crudo del request (headers + body)
 */
void login_user(struct io_uring *r, int f, const char *m, const char *b);

/*
 * me - handler de ejemplo de GET /api/me, ruta protegida
 *
 * Registrar con get_auth() (no get()), ver routes/index.h. Para cuando
 * dispatch() lo invoca ya paso la verificacion del JWT (utils/http/router.h):
 * responde los claims del token con jwt_claim(), sin volver a validar
 * nada. Sirve de plantilla para cualquier endpoint que necesite saber
 * quien es el usuario autenticado.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso
 *   b - buffer crudo del request, sin uso
 */
void me(struct io_uring *r, int f, const char *m, const char *b);

#endif
