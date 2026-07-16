/*
 * models/homeModel.h - SQL del endpoint de ejemplo/health-check
 *
 * NOMBRE
 *     homeModel.h - consulta usada por controllers/home.h (GET /api)
 *
 * DESCRIPCION
 *     A diferencia de models/sakila.*, esta consulta no es un prepared
 *     statement: controllers/home.h la lanza con db_query_async (texto
 *     plano), asi que no pasa por el registro de config/db.h.
 */
#pragma once

/* Nombre reservado para un eventual prepared statement de esta consulta;
 * no esta en uso (controllers/home.h llama a db_query_async con
 * QUERY_API_TIME directo, no a un prepared statement por nombre). */
#define STMT_API_TIME "api_get_time"

/* SELECT current_timestamp: prueba de vida de la conexion a Postgres. */
#define QUERY_API_TIME "SELECT current_timestamp;"
