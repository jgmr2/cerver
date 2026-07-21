/*
 * seed.h - genera sentencias INSERT con datos falsos para una PgTable ya
 * introspeccionada (ver schema.h), para la funcion "Datos de Prueba" de
 * la GUI (y del CLI, si algun dia hace falta).
 *
 * DESCRIPCION
 *     No hay libreria de datos falsos (tipo Faker) en C aca: una
 *     heuristica chica por udt_name de Postgres (y, cuando ayuda, por
 *     nombre de columna) alcanza para el proposito real de esto -- tener
 *     filas de prueba para ejercitar los endpoints generados, no para
 *     parecerse a produccion. Mismo criterio de "heuristica, no
 *     garantia" que ya documenta is_sensitive_column en codegen.c: un
 *     tipo custom/enum que la heuristica no reconozca puede hacer fallar
 *     el INSERT con un error de Postgres normal, que el llamador
 *     simplemente loguea.
 */
#ifndef DBFILLER_SEED_H
#define DBFILLER_SEED_H

#include "schema.h"

/*
 * Hash PBKDF2 precalculado con el mismo formato que usa
 * utils/auth/password.c ("iteraciones:salt_hex:hash_hex"). No es un
 * placeholder inutilizable: un usuario sembrado con este valor en su
 * columna sensible puede loguearse de verdad contra el backend real con
 * SEED_PASSWORD_PLAINTEXT como password. Salt fijo a proposito (no
 * aleatorio) -- aceptable porque esto es dato de prueba descartable, no
 * una cuenta real; ver el plan de esta sesion para el detalle de como se
 * calculo.
 */
#define SEED_PASSWORD_PLAINTEXT "SeedPassword123!"
#define SEED_PASSWORD_HASH "100000:00112233445566778899aabbccddeeff:355c26ea163e778ccc6e6fa2cf40d40ce993a188737758a8b82baa465be6c424"

/*
 * seed_build_inserts - arma un bloque de texto con una sentencia
 * "INSERT INTO <tabla> (...) VALUES (...), (...), ...;" para
 * 'row_count' filas de datos falsos.
 *
 * Columnas con is_pk && has_default (tipicamente SERIAL/IDENTITY) se
 * omiten de la lista de columnas del INSERT -- Postgres las genera
 * solas. Columnas sensibles (mismo filtro que codegen.c, expuesto ahora
 * como is_sensitive_column() no-static, ver codegen.h) reciben
 * SEED_PASSWORD_HASH en vez de pasar por la heuristica de tipo.
 *
 * Parametros:
 *   table     - tabla ya introspeccionada (pg_load_table)
 *   row_count - cuantas filas generar (se acota a un minimo de 1)
 *
 * Retorna:
 *   un buffer reservado con malloc() (el llamador libera con free()), o
 *   NULL si la tabla no tiene ninguna columna insertable (todas son PK
 *   con default, o la tabla no tiene columnas).
 */
char *seed_build_inserts(const PgTable *table, int row_count);

#endif
