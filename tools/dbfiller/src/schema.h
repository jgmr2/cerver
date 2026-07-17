#ifndef DBFILLER_SCHEMA_H
#define DBFILLER_SCHEMA_H

#include <stddef.h>

typedef enum {
    SQL_TYPE_INT,
    SQL_TYPE_BIGINT,
    SQL_TYPE_REAL,
    SQL_TYPE_TEXT,
    SQL_TYPE_VARCHAR,
    SQL_TYPE_BOOL,
    SQL_TYPE_DATE,
    SQL_TYPE_DATETIME,
    SQL_TYPE_ENUM,
    SQL_TYPE_BLOB,
    SQL_TYPE_UNKNOWN
} SqlType;

#define MAX_ENUM_VALUES 32
#define MAX_NAME_LEN 128

typedef struct {
    char name[MAX_NAME_LEN];
    SqlType sql_type;
    int varchar_len;       /* only meaningful for SQL_TYPE_VARCHAR, 0 = unbounded */

    int nullable;
    int is_pk;
    int is_autoincrement;
    int is_unique;

    int has_enum;
    char enum_values[MAX_ENUM_VALUES][MAX_NAME_LEN];
    int enum_count;

    int has_fk;
    char fk_table[MAX_NAME_LEN];
    char fk_column[MAX_NAME_LEN];
} DbColumn;

typedef struct {
    char name[MAX_NAME_LEN];
    DbColumn *columns;
    int column_count;
} DbTable;

void db_table_free(DbTable *table);

#endif
