#include "saved_connections.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0700)
#endif

int saved_connections_config_path(char *out, size_t out_size) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !appdata[0]) return -1;
    snprintf(out, out_size, "%s\\dbfiller\\connections.conf", appdata);
    return 0;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, out_size, "%s/dbfiller/connections.conf", xdg);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    snprintf(out, out_size, "%s/.config/dbfiller/connections.conf", home);
    return 0;
#endif
}

/* Resolves the engine ordinal from its saved label (db_engine_label's
   output), case-insensitively so a hand-edited file is forgiving. Falls
   back to SQLite - a saved file predating a future engine addition should
   never crash the parser, just point at something connectable. */
static DbEngine engine_from_label(const char *label) {
    for (int e = 0; e < DB_ENGINE_COUNT; e++)
        if (strcasecmp(label, db_engine_label((DbEngine)e)) == 0) return (DbEngine)e;
    return DB_ENGINE_SQLITE;
}

/* Splits "key=value" on the FIRST '=' only, so a value that itself contains
   '=' (a password, most likely) round-trips correctly. Trailing '\n'/'\r'
   must already be stripped by the caller. */
static int split_kv(char *line, char **key, char **value) {
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key = line;
    *value = eq + 1;
    return 1;
}

int saved_connections_load(SavedConnection *out, int max_count) {
    char path[600];
    if (saved_connections_config_path(path, sizeof(path)) != 0) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0; /* no saved connections yet - not an error */

    int count = 0;
    SavedConnection cur;
    memset(&cur, 0, sizeof(cur));
    int have_cur = 0;
    char line[2048];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        if (strcmp(line, "[connection]") == 0) {
            if (have_cur && count < max_count) out[count++] = cur;
            memset(&cur, 0, sizeof(cur));
            have_cur = 1;
            continue;
        }
        if (!have_cur) continue;

        char *key, *value;
        if (!split_kv(line, &key, &value)) continue;

        if (strcmp(key, "name") == 0) snprintf(cur.name, sizeof(cur.name), "%s", value);
        else if (strcmp(key, "engine") == 0) cur.engine = engine_from_label(value);
        else if (strcmp(key, "sqlite_path") == 0) snprintf(cur.sqlite_path, sizeof(cur.sqlite_path), "%s", value);
        else if (strcmp(key, "host") == 0) snprintf(cur.host, sizeof(cur.host), "%s", value);
        else if (strcmp(key, "port") == 0) cur.port = atoi(value);
        else if (strcmp(key, "user") == 0) snprintf(cur.user, sizeof(cur.user), "%s", value);
        else if (strcmp(key, "password") == 0) snprintf(cur.password, sizeof(cur.password), "%s", value);
    }
    if (have_cur && count < max_count) out[count++] = cur;

    fclose(f);
    return count;
}

int saved_connections_save(const SavedConnection *list, int count) {
    char path[600];
    if (saved_connections_config_path(path, sizeof(path)) != 0) return -1;

    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *backslash = strrchr(dir, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (slash) {
        *slash = '\0';
        if (MKDIR(dir) != 0 && errno != EEXIST) return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < count; i++) {
        fprintf(f, "[connection]\n");
        fprintf(f, "name=%s\n", list[i].name);
        fprintf(f, "engine=%s\n", db_engine_label(list[i].engine));
        fprintf(f, "sqlite_path=%s\n", list[i].sqlite_path);
        fprintf(f, "host=%s\n", list[i].host);
        fprintf(f, "port=%d\n", list[i].port);
        fprintf(f, "user=%s\n", list[i].user);
        fprintf(f, "password=%s\n", list[i].password);
    }

    fclose(f);
    return 0;
}
