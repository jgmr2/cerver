#include "db_backend.h"
#include "db_sqlite.h"
#ifdef DBFILLER_HAVE_MYSQL
#include "db_mysql.h"
#endif

#include <stddef.h>

const DbBackend *db_backend_for_engine(DbEngine engine) {
    switch (engine) {
        case DB_ENGINE_SQLITE: return &sqlite_backend;
#ifdef DBFILLER_HAVE_MYSQL
        case DB_ENGINE_MYSQL:
        case DB_ENGINE_MARIADB:
            return &mysql_backend;
#else
        case DB_ENGINE_MYSQL:
        case DB_ENGINE_MARIADB:
            return NULL; /* not built into this binary (no MySQL client at compile time) */
#endif
        /* Postgres/SQL Server backends land in later slices. */
        case DB_ENGINE_POSTGRES:
        case DB_ENGINE_MSSQL:
        default:
            return NULL;
    }
}

const char *db_engine_label(DbEngine engine) {
    switch (engine) {
        case DB_ENGINE_SQLITE: return "SQLite";
        case DB_ENGINE_POSTGRES: return "PostgreSQL";
        case DB_ENGINE_MYSQL: return "MySQL";
        case DB_ENGINE_MARIADB: return "MariaDB";
        case DB_ENGINE_MSSQL: return "SQL Server";
        default: return "?";
    }
}
