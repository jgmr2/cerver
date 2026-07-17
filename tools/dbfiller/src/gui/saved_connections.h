#ifndef DBFILLER_SAVED_CONNECTIONS_H
#define DBFILLER_SAVED_CONNECTIONS_H

#include <stddef.h>

#include "../db/db_backend.h"
#include "../schema.h"

#define MAX_SAVED_CONNECTIONS 32

/* One remembered connection profile, persisted as plain text (including the
   password - the user explicitly chose convenience over encryption/keyring
   integration for this local, single-user config file) at whatever path
   saved_connections_config_path() resolves to. */
typedef struct {
    char name[MAX_NAME_LEN];
    DbEngine engine;
    char sqlite_path[512];
    char host[128];
    int port;
    char user[128];
    char password[128];
} SavedConnection;

/* Fills out with the config file path ($XDG_CONFIG_HOME/dbfiller/connections.conf,
   or $HOME/.config/dbfiller/connections.conf, or %APPDATA%\dbfiller\connections.conf
   on Windows). Returns 0 on success, -1 if no usable base directory was found
   in the environment. */
int saved_connections_config_path(char *out, size_t out_size);

/* Loads every saved connection from disk into out[0..max_count). Returns the
   number loaded (0 if the file doesn't exist yet - not an error). */
int saved_connections_load(SavedConnection *out, int max_count);

/* Rewrites the whole config file from list[0..count) - there is no
   incremental update, the caller always saves its full in-memory list.
   Returns 0 on success, -1 on I/O error. */
int saved_connections_save(const SavedConnection *list, int count);

#endif
