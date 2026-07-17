#ifndef DBFILLER_DB_BACKEND_H
#define DBFILLER_DB_BACKEND_H

#include "../schema.h"

#define MAX_ERROR_LEN 512
#define MAX_TABLES 256

typedef enum {
    DB_VAL_NULL,
    DB_VAL_INT,
    DB_VAL_REAL,
    DB_VAL_TEXT
} DbValueType;

typedef struct {
    DbValueType type;
    long long as_int;
    double as_real;
    char *as_text; /* owned by caller for the lifetime of the insert call */
} DbValue;

#define MAX_PREVIEW_ROWS 50
#define MAX_PREVIEW_COLS 16
#define MAX_CELL_LEN 64

/* Read-only, paginated view of a table's actual rows, for display purposes
   only (not used by the generator/insert path). Every cell is stringified
   regardless of its SQL type; NULL is rendered as the literal "NULL". */
typedef struct {
    char col_names[MAX_PREVIEW_COLS][MAX_NAME_LEN];
    int col_count;
    char cells[MAX_PREVIEW_ROWS][MAX_PREVIEW_COLS][MAX_CELL_LEN];
    int row_count;
} RowPreview;

/* Opaque connection handle; each backend defines its own concrete struct
   and casts through this pointer. */
typedef struct DbConn DbConn;

/* Everything a GUI connection form or CLI flag set can produce. Server
   engines (Postgres/MySQL/MariaDB/SQL Server) use host/port/user/password
   (+ database once one is chosen); SQLite only uses `path`. */
typedef struct {
    char host[128];
    int port;
    char user[128];
    char password[128];
    char database[MAX_NAME_LEN]; /* empty when calling connect_server() */
    char path[512];              /* SQLite only */
} DbConnParams;

typedef struct {
    const char *name; /* e.g. "sqlite" */
    int default_port;  /* 0 for engines with no network port (SQLite) */

    /* Connects to a specific database (params.database or params.path). */
    DbConn *(*connect)(const DbConnParams *params, char *err, size_t err_len);

    /* Connects to the server without selecting a database, so the caller can
       list_databases() and let the user pick one. NULL for backends with no
       server concept (SQLite) — callers must check before calling. */
    DbConn *(*connect_server)(const DbConnParams *params, char *err, size_t err_len);

    void (*disconnect)(DbConn *conn);

    /* Fills out_names with up to max_tables database names on the server,
       using a connection made via connect_server(). NULL for SQLite. */
    int (*list_databases)(DbConn *conn, char out_names[][MAX_NAME_LEN], int max_tables);

    /* Fills out_names with up to max_tables table names, returns count. */
    int (*list_tables)(DbConn *conn, char out_names[][MAX_NAME_LEN], int max_tables);

    /* Populates *out_table (caller must db_table_free it). Returns 0 on success. */
    int (*get_table_schema)(DbConn *conn, const char *table_name, DbTable *out_table);

    int (*begin_tx)(DbConn *conn);
    int (*insert_row)(DbConn *conn, const char *table_name, const DbColumn *columns,
                       const DbValue *values, int column_count);
    int (*commit_tx)(DbConn *conn);
    int (*rollback_tx)(DbConn *conn);

    /* Samples one existing value at random from table_name.column_name, used
       to satisfy foreign keys. Returns 0 and fills out_value on success,
       -1 if the referenced table is empty or on error (see last_error). */
    int (*sample_column_value)(DbConn *conn, const char *table_name, const char *column_name,
                                DbValue *out_value);

    /* Generic paginated SELECT for display purposes (e.g. an Adminer-style
       "view records" panel). Returns 0 on success. */
    int (*fetch_rows)(DbConn *conn, const char *table_name, int offset, int limit,
                       RowPreview *out, char *err, size_t err_len);

    /* Total row count for table_name (SELECT COUNT(*)), so the "Registros"
       panel can show a page index ("Pagina X de Y") next to fetch_rows'
       paginated results. Returns 0 on success. */
    int (*count_rows)(DbConn *conn, const char *table_name, long long *out_count,
                       char *err, size_t err_len);

    /* Runs one arbitrary, user-typed SQL statement (the "Ejecutar SQL"
       panel). If it produces a result set (SELECT and friends), fills
       *out_rows and sets *out_is_query = 1 (truncated to
       MAX_PREVIEW_ROWS/MAX_PREVIEW_COLS, same as fetch_rows). Otherwise sets
       *out_is_query = 0 and *out_affected to the affected-row count. Returns
       0 on success. */
    int (*exec_sql)(DbConn *conn, const char *sql, int *out_is_query, RowPreview *out_rows,
                     int *out_affected, char *err, size_t err_len);

    /* Deletes every row from table_name ("Vaciar base de datos"). The caller
       is responsible for doing this in child-before-parent order (the
       reverse of db_graph_load_ordered's fill order) so foreign keys don't
       reject the delete. Returns 0 on success. */
    int (*delete_all_rows)(DbConn *conn, const char *table_name, char *err, size_t err_len);

    const char *(*last_error)(DbConn *conn);
} DbBackend;

typedef enum {
    DB_ENGINE_SQLITE,
    DB_ENGINE_POSTGRES,
    DB_ENGINE_MYSQL,
    DB_ENGINE_MARIADB,
    DB_ENGINE_MSSQL,
    DB_ENGINE_COUNT
} DbEngine;

/* Returns the backend for an engine, or NULL if not implemented yet
   (caller should show a "not implemented" message rather than crash). */
const DbBackend *db_backend_for_engine(DbEngine engine);
const char *db_engine_label(DbEngine engine);

#endif
