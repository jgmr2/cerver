/*
 * schema.h - representacion en memoria de una tabla de Postgres,
 * tal como la devuelve introspect.c
 *
 * DESCRIPCION
 *     Deliberadamente no modela foreign keys ni enums: el generador de
 *     CRUD (codegen.c) no los necesita (ver tools/dbfiller/README.md /
 *     el plan de esta sesion) - una columna FK viaja como cualquier
 *     otro parametro de texto, y si viola la constraint Postgres
 *     devuelve error, que el endpoint generado mapea a 409.
 */
#ifndef DBFILLER_SCHEMA_H
#define DBFILLER_SCHEMA_H

#define MAX_NAME_LEN 128
#define MAX_COLUMNS 256

typedef struct {
    char name[MAX_NAME_LEN];
    char data_type[MAX_NAME_LEN]; /* udt_name/data_type de information_schema, solo informativo (va en comentarios) */
    int nullable;
    int has_default;   /* column_default IS NOT NULL: excluida de INSERT/UPDATE, ver codegen.c */
    int is_pk;
} PgColumn;

typedef struct {
    char name[MAX_NAME_LEN];
    PgColumn columns[MAX_COLUMNS];
    int column_count;
    int pk_index; /* indice en columns[] de la (unica) PK, -1 si no hay o hay mas de una */
} PgTable;

#endif
