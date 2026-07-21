#include "dbfiller_state.h"
#include <string.h>

PGconn *g_conn = NULL;
pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;
char g_repo_root[DBFILLER_REPO_ROOT_LEN] = "";
char g_database_url[DBFILLER_DB_URL_LEN] = "";

const char *dbfiller_repo_root(void) {
    return g_repo_root[0] ? g_repo_root : ".";
}
