#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "../../third_party/nuklear.h"

#define NK_GLFW_GL2_IMPLEMENTATION
#include "../../third_party/nuklear_glfw_gl2.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#endif

#include "../db/db_backend.h"
#include "../db_graph.h"
#include "../generator.h"
#include "saved_connections.h"
#ifndef _WIN32
#include "docker_control.h"
#endif

#define WIN_W 760
#define WIN_H 640
#define LOG_CAP 8192
#define SQL_TEXT_CAP 8192
#define SQL_SUGGEST_MAX 8
#define SQL_SCHEMA_CACHE_MAX_COLUMNS 64
#define AUTO_REFRESH_INTERVAL 2.0 /* seconds between background table/schema refreshes */
#define THEME_CHECK_INTERVAL 3.0  /* seconds between OS light/dark preference checks */

/* Cached column names for one table, used by the SQL tab's autocomplete to
   suggest fields (and to resolve "alias." / "table." completions) without
   re-querying the schema on every keystroke. Populated by
   sql_refresh_schema_cache(). */
typedef struct {
    char name[MAX_NAME_LEN];
    char columns[SQL_SCHEMA_CACHE_MAX_COLUMNS][MAX_NAME_LEN];
    int column_count;
} SqlSchemaCache;

/* What kind of thing a suggestion represents, so the floating list can give
   each entry a distinct visual cue (see the colored left bar in the popup
   drawing code) instead of one undifferentiated list. */
typedef enum {
    SQL_SUG_KEYWORD,
    SQL_SUG_TABLE,
    SQL_SUG_COLUMN,
    SQL_SUG_FUNCTION
} SqlSuggestKind;

typedef struct {
    const char *text; /* stable for the frame: a literal, or into st->tables[]/sql_schema_cache */
    SqlSuggestKind kind;
} SqlSuggestion;

/* Which content the main panel shows. The sidebar (saved connections tree +
   pinned "Nueva conexion"/"Contenedores" rows) is always visible regardless
   of this - see draw_sidebar()/draw_ui(). */
typedef enum {
    VIEW_NEW_CONNECTION, /* login form: fill in a new connection */
    VIEW_CONNECTION,     /* connected header + database/table detail */
    VIEW_CONTAINERS      /* Docker container management (Linux only) */
} AppViewMode;

typedef struct {
    DbEngine engine;
    int connected;      /* connected to a specific database, tables[] is valid */
    int picking_db;      /* connected to the server, waiting for a database pick */
    AppViewMode view_mode;

    char sqlite_path[512];
    char host[128];
    char port_str[16];
    char user[128];
    char password[128];

    char status_msg[256];

    const DbBackend *backend;
    DbConn *conn;

    char databases[MAX_TABLES][MAX_NAME_LEN];
    int database_count;

    char tables[MAX_TABLES][MAX_NAME_LEN];
    int table_count;

    char count_str[16];
    char result_log[LOG_CAP];
    char connection_summary[256];

    /* Sidebar / table-detail navigation state. Only one database can be
       "active" at a time since there is a single live DbConn; expanding a
       different database in the sidebar reconnects to it (accordion-style,
       like a VS Code folder tree where only one root stays open). */
    char active_database[MAX_NAME_LEN];
    char selected_table[MAX_NAME_LEN]; /* empty = database-level view */
    DbTable selected_schema;
    int schema_valid;
    int table_view_mode; /* 0 = Estructura, 1 = Registros, 2 = SQL */
    RowPreview *row_preview;
    int preview_offset;
    long long preview_total_rows; /* -1 = unknown (count_rows failed), else total row count for the current table */
    char single_count_str[16];
    char table_result_msg[256];

    /* In-app file browser for the SQLite path (self-contained: no external
       process, so nothing to hang or fail to find on the host system). */
    int show_browser;
    char browse_dir[512];
    char browse_dirs[128][MAX_NAME_LEN];
    int browse_dir_count;
    char browse_files[128][MAX_NAME_LEN];
    int browse_file_count;

    /* Theme + keyboard-navigation bookkeeping. */
    int dark_theme;
    int focused_field;  /* logical field id with NK_EDIT_ACTIVE this frame, -1 if none */
    int pending_focus;  /* field id to force-focus this frame via nk_edit_focus, -1 if none */
    int field_count;    /* focusable fields drawn last frame, used to wrap Tab */
    int sidebar_collapsed;

    double last_refresh_time; /* glfwGetTime() of the last table/schema refresh, for auto-refresh */
    double last_theme_check_time; /* glfwGetTime() of the last OS theme poll */

    /* "SQL" tab: a free-form statement editor + results viewer, living
       alongside Estructura/Registros in the table-detail segmented control.
       sql_edit is a persistent nk_text_edit (not just a string) so we can
       read its live cursor position for suggest-as-you-type; sql_text is
       just the fixed backing memory it writes into - read it back out via
       sql_text_snapshot(), never index into it directly. */
    struct nk_text_edit sql_edit;
    char sql_text[SQL_TEXT_CAP];
    int sql_is_query;
    int sql_affected;
    RowPreview *sql_result;
    char sql_status_msg[256];

    /* Pagination for SELECT-shaped results: sql_base_query is the trimmed
       statement last run via "Ejecutar" (without a trailing ';'), reused by
       Anterior/Siguiente to re-run the same query at a different offset
       without the user retyping anything. sql_pageable is 0 for statements
       we can't safely wrap in "SELECT * FROM (...) LIMIT/OFFSET" (anything
       that isn't SELECT/WITH, or where wrapping itself failed) - the pager
       just doesn't show in that case. sql_total_rows is -1 when unknown
       (the COUNT(*) wrapper query failed) rather than 0, so the UI can tell
       "no rows" apart from "couldn't count". */
    char sql_base_query[SQL_TEXT_CAP];
    int sql_offset;
    int sql_pageable;
    long long sql_total_rows;

    /* Column names per table, for the SQL tab's autocomplete - see
       SqlSchemaCache. Heap-allocated (like row_preview/sql_result above)
       rather than embedded here, since AppState itself lives on main()'s
       stack and MAX_TABLES * SqlSchemaCache is a couple MB. */
    SqlSchemaCache *sql_schema_cache;
    int sql_schema_cache_count;

    /* Suggest-as-you-type state, carried across frames: the word range the
       current suggestion list was computed for (so we know whether to keep
       or reset sql_suggest_index), the list itself, and which entry
       Tab/Down has cycled to. Entries' `text` point at static keyword
       strings, st->tables[], or st->sql_schema_cache[] - all stable for the
       frame's lifetime, never at text inside sql_edit itself. */
    int sql_word_start;
    int sql_word_len;
    int sql_suggest_count;
    int sql_suggest_index;
    SqlSuggestion sql_suggestions[SQL_SUGGEST_MAX];

    /* "Vaciar base de datos" is destructive, so it needs an explicit second
       click on a confirmation prompt instead of firing immediately. */
    int confirm_wipe;

    /* Material Icons glyphs baked as a second atlas font (see load_font_file
       in main()). NULL if the font file couldn't be found - draw_icon()
       silently skips drawing in that case, buttons just show their text
       label with no icon rather than crashing or showing tofu. */
    const struct nk_user_font *icon_font;

    /* Saved connection profiles (sidebar top level, persisted to disk - see
       saved_connections.h). Heap-allocated like row_preview/sql_result/
       sql_schema_cache above, same "keep AppState's own stack frame small"
       rationale. active_saved_connection_index is -1 when the live
       connection (if any) was made ad-hoc from "Nueva conexion" and never
       saved - see the synthetic "Conexion actual" sidebar row in
       draw_sidebar(). save_conn_pending/save_conn_name back the two-step
       "Guardar conexion" flow (same shape as confirm_wipe above). */
    SavedConnection *saved_connections;
    int saved_connection_count;
    int active_saved_connection_index;
    int save_conn_pending;
    char save_conn_name[MAX_NAME_LEN];

    /* Status line for VIEW_CONTAINERS, declared unconditionally (unlike the
       fields below) so the shared top-of-window status row in draw_ui can
       reference it without #ifdef'ing that shared code path - harmless
       unused space on a Windows build, where VIEW_CONTAINERS is simply
       never reachable. */
    char docker_status_msg[256];

#ifndef _WIN32
    /* Docker container list for the "Contenedores" view (Linux only - see
       docker_control.h). docker_confirm_stop_index mirrors confirm_wipe's
       two-step pattern, scoped to a single row (-1 = none armed). */
    DockerContainer *docker_containers;
    int docker_container_count;
    int docker_confirm_stop_index;
#endif
} AppState;

static const char *ENGINE_LABELS[DB_ENGINE_COUNT] = {
    "SQLite", "PostgreSQL", "MySQL", "MariaDB", "SQL Server"
};
static const int ENGINE_DEFAULT_PORTS[DB_ENGINE_COUNT] = {
    0, 5432, 3306, 3306, 1433
};

/* Suggest-as-you-type candidates for the SQL tab. sql_collect_suggestions()
   picks which of these lists to search based on the syntactic context
   around the cursor (see SqlContext/sql_analyze_context): a short
   "PRIMARY" list of the most likely next keywords, plus - only while the
   user is actively typing a prefix - a longer "EXTRA" list of every OTHER
   keyword that is still grammatically valid in that same context. There is
   deliberately no global "all keywords" fallback: showing e.g. CREATE TABLE
   while the user is typing inside a WHERE clause is a confirmed-wrong
   suggestion, not just an unlikely one, so it must never appear regardless
   of how empty the candidate list is. Real table/column names are matched
   in from st->tables[] and st->sql_schema_cache[] separately, also gated by
   context (see sql_collect_suggestions's suggest_tables/suggest_columns). */

/* Right at the start of a statement (or right after ';'): only a verb makes
   sense - nothing else is valid SQL here. */
static const char *SQL_KW_STATEMENT_START[] = {
    "SELECT", "INSERT INTO", "UPDATE", "DELETE FROM", "CREATE TABLE",
    "DROP TABLE", "ALTER TABLE", "WITH", "EXPLAIN", "PRAGMA",
    "BEGIN TRANSACTION", "COMMIT", "ROLLBACK"
};
static const char *SQL_KW_STATEMENT_START_EXTRA[] = {
    "CREATE INDEX", "CREATE UNIQUE INDEX", "CREATE VIEW",
    "DROP INDEX", "DROP VIEW", "TRUNCATE TABLE", "SAVEPOINT"
};
/* Between SELECT/DISTINCT and FROM. Kept short (SQL_SUGGEST_MAX is only 8
   slots) so there's still room left for actual column names - see
   sql_collect_suggestions's columns_first handling below. */
static const char *SQL_KW_SELECT_LIST[] = { "*", "DISTINCT", "FROM", "COUNT(*)", "CASE" };
/* Other tokens that can legally follow SELECT/DISTINCT/a column expression,
   before FROM: aggregate/scalar functions, ALL, AS (for aliasing an
   expression). Never a clause keyword like WHERE/GROUP BY - those can't
   appear before FROM even exists. */
static const char *SQL_KW_SELECT_LIST_EXTRA[] = {
    "ALL", "AS", "COUNT", "SUM", "AVG", "MIN", "MAX",
    "COALESCE", "NULLIF", "CAST", "SUBSTR", "LENGTH", "TRIM", "UPPER",
    "LOWER", "REPLACE", "CONCAT", "ROUND", "NOW()", "CURRENT_TIMESTAMP",
    "DATE", "STRFTIME", "ROW_NUMBER()", "RANK()", "DENSE_RANK()",
    "OVER", "PARTITION BY"
};
/* Right after a table/alias reference has just been parsed (e.g. right
   after "FROM users" or "FROM users u") - what commonly comes next, most
   likely first. */
static const char *SQL_KW_AFTER_TABLE[] = {
    "WHERE", "JOIN", "LEFT JOIN", "INNER JOIN", "GROUP BY", "ORDER BY", "LIMIT"
};
static const char *SQL_KW_AFTER_TABLE_EXTRA[] = {
    "LEFT OUTER JOIN", "RIGHT JOIN", "RIGHT OUTER JOIN", "FULL JOIN",
    "FULL OUTER JOIN", "CROSS JOIN", "ON", "HAVING", "OFFSET",
    "UNION", "UNION ALL", "INTERSECT", "EXCEPT", "AS"
};
/* WHERE / AND / OR / HAVING / ON - a column is almost always the actual
   next token here, so these are just the few keywords worth showing
   alongside it (see columns_first below), not an exhaustive list. */
static const char *SQL_KW_CONDITION[] = {
    "AND", "OR", "NOT", "IS NULL", "LIKE", "IN", "BETWEEN", "EXISTS"
};
static const char *SQL_KW_CONDITION_EXTRA[] = {
    "IS NOT NULL", "NOT IN", "NOT LIKE", "NOT EXISTS",
    "COALESCE", "CAST", "UPPER", "LOWER", "LENGTH", "SUBSTR", "TRIM"
};
/* GROUP BY / ORDER BY - likewise, a column comes first almost always, and
   ASC/DESC are the only other tokens that make sense right there. */
static const char *SQL_KW_COLUMN_LIST[] = { "ASC", "DESC" };
/* Both GROUP BY and ORDER BY map to the same context (sql_analyze_context
   doesn't distinguish which one introduced it), so this covers what can
   validly follow either: HAVING only really belongs after GROUP BY, but
   offering it after ORDER BY too is a harmless loosening, not a wrong
   suggestion - "drives predictive typing, not validation" (see the comment
   above sql_analyze_context). */
static const char *SQL_KW_COLUMN_LIST_EXTRA[] = { "HAVING", "ORDER BY", "LIMIT", "OFFSET" };
/* "INSERT INTO t" - a WHERE/JOIN/GROUP BY clause is nonsensical here (those
   are SELECT-only), so this gets its own keyword set entirely separate from
   SQL_KW_AFTER_TABLE. A column list in parens isn't modeled (no paren
   tracking), so VALUES is the one thing worth hinting. */
static const char *SQL_KW_AFTER_INSERT_TABLE[] = { "VALUES" };
/* "UPDATE t" - only SET is valid next (still no WHERE/JOIN/etc: those only
   make sense once already inside the SET-list, after SQL_CTX_ASSIGNMENT). */
static const char *SQL_KW_AFTER_UPDATE_TABLE[] = { "SET" };
/* "SET col = val" (or right after SET, before the first column) - once a
   value has been assigned, WHERE is the natural next clause; a comma for
   another "col = val" pair is handled like any other column context. */
static const char *SQL_KW_ASSIGNMENT_EXTRA[] = { "WHERE" };

/* Reserved words that can appear right after a table reference without
   being an alias for it (JOIN, WHERE, ...), so the alias-binding scan in
   sql_analyze_context doesn't mistake them for an alias. Deliberately a
   flat single-token list (not the multi-word entries above) since it's
   matched token-by-token. */
static const char *SQL_RESERVED_TOKENS[] = {
    "SELECT", "FROM", "WHERE", "JOIN", "INNER", "LEFT", "RIGHT", "FULL",
    "CROSS", "OUTER", "ON", "GROUP", "ORDER", "BY", "HAVING", "LIMIT",
    "OFFSET", "AS", "AND", "OR", "NOT", "SET", "VALUES", "INTO", "UPDATE",
    "INSERT", "DELETE", "CREATE", "ALTER", "DROP", "TABLE", "DISTINCT",
    "UNION", "ALL", "IS", "NULL", "IN", "LIKE", "BETWEEN", "EXISTS",
    "CASE", "WHEN", "THEN", "ELSE", "END", "WITH"
};
#define SQL_RESERVED_TOKENS_COUNT ((int)(sizeof(SQL_RESERVED_TOKENS) / sizeof(SQL_RESERVED_TOKENS[0])))

#ifndef _WIN32
/* Runs a shell command and checks its stdout for "dark"/"light". Returns
   -1 (inconclusive - command missing, empty output, or neither substring
   present) rather than guessing, so callers can fall through to the next
   detection method instead of locking onto a wrong answer. */
static int shell_prefers_dark(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    (void)n;
    if (strstr(buf, "dark") || strstr(buf, "Dark")) return 1;
    if (strstr(buf, "light") || strstr(buf, "Light") || strstr(buf, "default")) return 0;
    return -1;
}
#endif

/* Best-effort detection of the OS light/dark preference. Always overridable
   from the UI, since detection is inherently fragile (especially on Linux,
   where there's no single portable API for this - gsettings only speaks
   for GNOME/Cinnamon/Budgie-family desktops, so KDE Plasma gets its own
   check via kreadconfig; anything else falls back to dark). */
static int os_prefers_dark_theme(void) {
#ifdef _WIN32
    DWORD value = 0, size = sizeof(value);
    LONG r = RegGetValueA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size);
    if (r != ERROR_SUCCESS) return 1; /* default to dark when undetectable */
    return value == 0; /* 0 = dark, 1 = light */
#else
    int r = shell_prefers_dark("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
    if (r >= 0) return r;
    r = shell_prefers_dark(
        "kreadconfig5 --file kdeglobals --group General --key ColorScheme 2>/dev/null || "
        "kreadconfig6 --file kdeglobals --group General --key ColorScheme 2>/dev/null");
    if (r >= 0) return r;
    return 1; /* undetectable on this desktop: default to dark */
#endif
}

#ifdef _WIN32
/* Windows' native "Open File" common dialog. Returns 1 and fills out_path
   on success, 0 if the user cancelled (the caller falls back to the in-app
   browser in that case, so this never blocks picking a file entirely). */
static int native_open_file_dialog(char *out_path, size_t out_size, const char *initial_dir) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrFilter = "Bases SQLite\0*.db;*.sqlite;*.sqlite3\0Todos los archivos\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = (initial_dir && initial_dir[0]) ? initial_dir : NULL;
    ofn.lpstrTitle = "Elegir archivo SQLite";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return 0;

    /* Normalize to forward slashes - the rest of the app (basename via
       strrchr(path, '/'), the in-app browser) assumes '/', and sqlite3
       itself accepts either separator on Windows. */
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    snprintf(out_path, out_size, "%s", buf);
    return 1;
}
#else
/* Runs argv[0] with the given NULL-terminated argument list, capturing its
   stdout (trimmed of a trailing newline) into `out`. Deliberately uses
   fork+execvp rather than popen/system: the picked directory can contain
   anything the user typed into the path field, and building a shell
   command string out of that would be a command-injection footgun -
   execvp takes each argument literally, no shell involved. Returns 1 on
   exit status 0 with non-empty output (a file was picked), 0 otherwise
   (cancelled, or the tool isn't installed). */
static int run_and_capture_argv(char *const argv[], char *out, size_t out_size) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], argv);
        _exit(127); /* argv[0] not found */
    }
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < out_size - 1 && (n = read(pipefd[0], out + total, out_size - 1 - total)) > 0)
        total += (size_t)n;
    out[total] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r')) out[--total] = '\0';
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && out[0] != '\0';
}

/* zenity (GNOME/GTK desktops) first, then kdialog (KDE/Qt) - same
   best-effort, try-the-next-one-if-missing pattern as os_prefers_dark_theme.
   Neither installed means native_open_file_dialog returns 0 and the caller
   falls back to dbfiller's own in-app browser. */
static int native_open_file_dialog(char *out_path, size_t out_size, const char *initial_dir) {
    char filename_arg[600];
    snprintf(filename_arg, sizeof(filename_arg), "--filename=%s/",
             (initial_dir && initial_dir[0]) ? initial_dir : ".");
    char *zenity_argv[] = {
        (char *)"zenity", (char *)"--file-selection",
        (char *)"--title=Elegir archivo SQLite",
        filename_arg,
        (char *)"--file-filter=Bases SQLite | *.db *.sqlite *.sqlite3",
        (char *)"--file-filter=Todos los archivos | *",
        NULL
    };
    if (run_and_capture_argv(zenity_argv, out_path, out_size)) return 1;

    char kdialog_dir[600];
    snprintf(kdialog_dir, sizeof(kdialog_dir), "%s/", (initial_dir && initial_dir[0]) ? initial_dir : ".");
    char *kdialog_argv[] = {
        (char *)"kdialog", (char *)"--getopenfilename", kdialog_dir,
        (char *)"*.db *.sqlite *.sqlite3|Bases SQLite\n*|Todos los archivos",
        NULL
    };
    if (run_and_capture_argv(kdialog_argv, out_path, out_size)) return 1;

    return 0;
}
#endif

/* Flat Material-style palette: opaque surfaces, no translucency. Dark theme
   follows Material Dark's two-tone elevation (a near-black canvas with
   lighter #1E1E1E cards on top, pushed per-panel in draw_ui via
   push_surface_bg); light theme is white cards on a soft gray canvas. */
#define ACCENT_R 66
#define ACCENT_G 133
#define ACCENT_B 244

static void build_flat_table(struct nk_color t[NK_COLOR_COUNT], int dark) {
    if (dark) {
        t[NK_COLOR_TEXT] = nk_rgb(230, 230, 230);
        t[NK_COLOR_WINDOW] = nk_rgb(18, 18, 18);
        t[NK_COLOR_HEADER] = nk_rgb(30, 30, 30);
        t[NK_COLOR_BORDER] = nk_rgba(255, 255, 255, 60);
        t[NK_COLOR_BUTTON] = nk_rgb(45, 45, 45);
        t[NK_COLOR_BUTTON_HOVER] = nk_rgb(58, 58, 58);
        t[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(70, 70, 70);
        t[NK_COLOR_TOGGLE] = nk_rgb(50, 50, 50);
        t[NK_COLOR_TOGGLE_HOVER] = nk_rgb(65, 65, 65);
        t[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_SELECT] = nk_rgb(45, 45, 45);
        t[NK_COLOR_SELECT_ACTIVE] = nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 140);
        t[NK_COLOR_SLIDER] = nk_rgb(45, 45, 45);
        t[NK_COLOR_SLIDER_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(90, 150, 250);
        t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(45, 110, 220);
        t[NK_COLOR_PROPERTY] = nk_rgb(45, 45, 45);
        t[NK_COLOR_EDIT] = nk_rgb(40, 40, 40);
        t[NK_COLOR_EDIT_CURSOR] = nk_rgb(230, 230, 230);
        t[NK_COLOR_COMBO] = nk_rgb(40, 40, 40);
        t[NK_COLOR_CHART] = nk_rgb(40, 40, 40);
        t[NK_COLOR_CHART_COLOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(240, 100, 100);
        t[NK_COLOR_SCROLLBAR] = nk_rgb(24, 24, 24);
        t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(75, 75, 75);
        t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(90, 90, 90);
        t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(105, 105, 105);
        t[NK_COLOR_TAB_HEADER] = nk_rgb(35, 35, 35);
        t[NK_COLOR_KNOB] = nk_rgb(45, 45, 45);
        t[NK_COLOR_KNOB_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_KNOB_CURSOR_HOVER] = nk_rgb(90, 150, 250);
        t[NK_COLOR_KNOB_CURSOR_ACTIVE] = nk_rgb(45, 110, 220);
    } else {
        t[NK_COLOR_TEXT] = nk_rgb(30, 30, 30);
        t[NK_COLOR_WINDOW] = nk_rgb(245, 245, 245);
        t[NK_COLOR_HEADER] = nk_rgb(255, 255, 255);
        t[NK_COLOR_BORDER] = nk_rgba(0, 0, 0, 55);
        t[NK_COLOR_BUTTON] = nk_rgb(233, 233, 233);
        t[NK_COLOR_BUTTON_HOVER] = nk_rgb(222, 222, 222);
        t[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(208, 208, 208);
        t[NK_COLOR_TOGGLE] = nk_rgb(220, 220, 220);
        t[NK_COLOR_TOGGLE_HOVER] = nk_rgb(200, 200, 200);
        t[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_SELECT] = nk_rgb(233, 233, 233);
        t[NK_COLOR_SELECT_ACTIVE] = nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 60);
        t[NK_COLOR_SLIDER] = nk_rgb(220, 220, 220);
        t[NK_COLOR_SLIDER_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(90, 150, 250);
        t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(45, 110, 220);
        t[NK_COLOR_PROPERTY] = nk_rgb(233, 233, 233);
        t[NK_COLOR_EDIT] = nk_rgb(238, 238, 238);
        t[NK_COLOR_EDIT_CURSOR] = nk_rgb(30, 30, 30);
        t[NK_COLOR_COMBO] = nk_rgb(238, 238, 238);
        t[NK_COLOR_CHART] = nk_rgb(233, 233, 233);
        t[NK_COLOR_CHART_COLOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(200, 50, 50);
        t[NK_COLOR_SCROLLBAR] = nk_rgb(238, 238, 238);
        t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(190, 190, 190);
        t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(170, 170, 170);
        t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(150, 150, 150);
        t[NK_COLOR_TAB_HEADER] = nk_rgb(225, 225, 225);
        t[NK_COLOR_KNOB] = nk_rgb(220, 220, 220);
        t[NK_COLOR_KNOB_CURSOR] = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B);
        t[NK_COLOR_KNOB_CURSOR_HOVER] = nk_rgb(90, 150, 250);
        t[NK_COLOR_KNOB_CURSOR_ACTIVE] = nk_rgb(45, 110, 220);
    }
}

/* Material-ish metrics: small (4-6px) corner radii instead of pill-shaped
   glass rounding, thin or no borders (flat surfaces separate by color/
   elevation, not by a drawn edge), and explicit selectable/combo states so
   list rows and the engine dropdown get a clean "state layer" (a flat
   overlay wash on hover/press, matching Material's ripple/hover spec)
   instead of nuklear's default no-hover-feedback behavior. */
static void apply_flat_metrics(struct nk_context *ctx, int dark) {
    struct nk_style *s = &ctx->style;

    s->window.rounding = 6.0f;
    s->window.border = 0.0f;
    s->window.group_border = 1.5f;
    s->window.combo_border = 1.5f;
    s->window.popup_border = 1.5f;
    s->window.padding = nk_vec2(12, 12);
    s->window.group_padding = nk_vec2(12, 12);
    s->window.popup_padding = nk_vec2(8, 8);
    s->window.combo_padding = nk_vec2(8, 6);
    s->window.spacing = nk_vec2(8, 10);
    s->window.scrollbar_size = nk_vec2(10, 10);

    s->button.rounding = 4.0f;
    s->button.border = 1.0f;

    s->edit.rounding = 4.0f;
    s->edit.border = 1.5f;

    s->combo.rounding = 4.0f;
    s->combo.border = 1.5f;
    s->combo.button.rounding = 4.0f;

    s->checkbox.border = 1.0f;
    s->option.border = 1.0f;

    s->progress.rounding = 4.0f;
    s->progress.cursor_rounding = 4.0f;

    s->scrollh.rounding = 6.0f;
    s->scrollh.rounding_cursor = 6.0f;
    s->scrollh.border = 0.0f;
    s->scrollv.rounding = 6.0f;
    s->scrollv.rounding_cursor = 6.0f;
    s->scrollv.border = 0.0f;

    s->tab.rounding = 4.0f;
    s->knob.border = 1.0f;
    s->slider.rounding = 4.0f;

    /* Selectable rows (tree items): idle is invisible (shows the card's
       flat surface color underneath), hover/press are a flat state-layer
       overlay, and selected gets a translucent accent wash - all state
       layers are alpha overlays on top of a *known opaque* surface color,
       which is the standard Material "state layer" technique (unlike the
       old glass theme, where the layer beneath was an unpredictable real
       desktop). */
    s->selectable.rounding = 4.0f;
    s->selectable.padding = nk_vec2(8.0f, 6.0f);
    if (dark) {
        s->selectable.normal = nk_style_item_color(nk_rgba(255, 255, 255, 0));
        s->selectable.hover = nk_style_item_color(nk_rgba(255, 255, 255, 20));
        s->selectable.pressed = nk_style_item_color(nk_rgba(255, 255, 255, 32));
        s->selectable.normal_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 60));
        s->selectable.hover_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 75));
        s->selectable.pressed_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 90));
    } else {
        s->selectable.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
        s->selectable.hover = nk_style_item_color(nk_rgba(0, 0, 0, 16));
        s->selectable.pressed = nk_style_item_color(nk_rgba(0, 0, 0, 28));
        s->selectable.normal_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 40));
        s->selectable.hover_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 55));
        s->selectable.pressed_active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 70));
    }

    /* The engine combo's dropdown popup uses contextual_button per row;
       give it an explicit opaque background and accent hover, instead of
       nk_style_from_table's default (a near-invisible wash of the window
       color), so it always reads as a real menu. */
    if (dark) {
        s->contextual_button.normal = nk_style_item_color(nk_rgb(40, 40, 40));
        s->contextual_button.hover = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 90));
        s->contextual_button.active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 130));
    } else {
        s->contextual_button.normal = nk_style_item_color(nk_rgb(255, 255, 255));
        s->contextual_button.hover = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 55));
        s->contextual_button.active = nk_style_item_color(nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B, 90));
    }
}

static void apply_theme(struct nk_context *ctx, int dark) {
    struct nk_color table[NK_COLOR_COUNT];
    build_flat_table(table, dark);
    nk_style_from_table(ctx, table);
    apply_flat_metrics(ctx, dark);
}

/* Card elevation (Material's defining trait): push a lighter (dark theme) or
   white (light theme) surface color before a top-level nk_group_begin (the
   sidebar, the main panel, the file browser) so it reads as a raised card
   against the darker/grayer canvas, then pop right after nk_group_end. */
static struct nk_color surface_color(int dark) {
    return dark ? nk_rgb(30, 30, 30) : nk_rgb(255, 255, 255);
}
static void push_surface_bg(struct nk_context *ctx, int dark) {
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                              nk_style_item_color(surface_color(dark)));
}
static void pop_surface_bg(struct nk_context *ctx) {
    nk_style_pop_style_item(ctx);
}

/* Theme-aware text colors, used instead of one-off hardcoded nk_rgb() calls
   so status/warning/header text keeps enough contrast against both the dark
   and light surface colors. */
static struct nk_color status_text_color(int dark) {
    return dark ? nk_rgb(235, 195, 90) : nk_rgb(150, 92, 8);
}
static struct nk_color accent_text_color(int dark) {
    return dark ? nk_rgb(150, 200, 255) : nk_rgb(20, 85, 165);
}
static struct nk_color muted_text_color(int dark) {
    return dark ? nk_rgba(240, 240, 245, 150) : nk_rgba(25, 25, 35, 140);
}
static struct nk_color normal_icon_color(int dark) {
    return dark ? nk_rgb(230, 230, 230) : nk_rgb(30, 30, 30);
}

/* Solid accent style for primary actions (Conectar, Llenar toda la base de
   datos, Generar, the active tab of a segmented control) - everything else
   stays in the flat neutral button style, so these read as "the one thing
   to click" instead of every button competing for attention at the same
   visual weight. One fixed saturated blue works on both themes, so the
   accent itself doesn't need a dark/light variant, only the button text
   (always white, for contrast against the solid fill). Must be paired with
   pop_button_style() after the widget call(s). */
static void push_accent_button(struct nk_context *ctx) {
    nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(nk_rgba(66, 133, 244, 235)));
    nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(nk_rgba(90, 150, 250, 245)));
    nk_style_push_style_item(ctx, &ctx->style.button.active, nk_style_item_color(nk_rgba(45, 110, 220, 255)));
    nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgba(35, 95, 200, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_hover, nk_rgb(255, 255, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_active, nk_rgb(255, 255, 255));
}

static void pop_button_style(struct nk_context *ctx) {
    nk_style_pop_color(ctx);      /* text_active */
    nk_style_pop_color(ctx);      /* text_hover */
    nk_style_pop_color(ctx);      /* text_normal */
    nk_style_pop_color(ctx);      /* border_color */
    nk_style_pop_style_item(ctx); /* active */
    nk_style_pop_style_item(ctx); /* hover */
    nk_style_pop_style_item(ctx); /* normal */
}

/* Same idea as push_accent_button but red, for irreversible/destructive
   actions ("Vaciar base de datos"'s confirm step) - a distinct color
   language so a destructive action never looks like a routine one. Must be
   paired with pop_button_style() (they push/pop the same style slots). */
static void push_danger_button(struct nk_context *ctx) {
    nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(nk_rgba(211, 47, 47, 235)));
    nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(nk_rgba(229, 70, 70, 245)));
    nk_style_push_style_item(ctx, &ctx->style.button.active, nk_style_item_color(nk_rgba(180, 35, 35, 255)));
    nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgba(150, 25, 25, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_hover, nk_rgb(255, 255, 255));
    nk_style_push_color(ctx, &ctx->style.button.text_active, nk_rgb(255, 255, 255));
}

/* Material Icons glyphs, addressed by their stable Private Use Area
   codepoint in third_party/fonts/MaterialIcons-Regular.otf. Verified by
   rendering each candidate codepoint to a PNG and visually inspecting it
   (the font's codepoint layout doesn't match the icon names in casual
   documentation, so guessing from memory produced wrong icons - this list
   is the confirmed-correct result). */
#define ICON_CHEVRON_LEFT  0xE09Du
#define ICON_CHEVRON_RIGHT 0xE09Cu
#define ICON_REFRESH       0xE0C1u
#define ICON_CLOSE         0xE168u
#define ICON_DELETE        0xE1B9u
#define ICON_WARNING       0xE52Du
#define ICON_SEARCH        0xE286u
#define ICON_PLAY          0xE361u
#define ICON_ADD           0xE047u
#define ICON_FOLDER        0xE2A3u

/* All our icon codepoints live in the Basic Multilingual Plane's Private
   Use Area (U+E000-U+F8FF), which always encodes to exactly 3 UTF-8 bytes -
   so this doesn't need to be a general-purpose UTF-8 encoder. */
static int icon_utf8(char out[4], unsigned int codepoint) {
    out[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (char)(0x80 | (codepoint & 0x3F));
    out[3] = '\0';
    return 3;
}

/* Draws one icon glyph at an explicit position/size - nk_draw_text itself
   is top-left aligned, not centered, so both draw_icon_left (icon+label
   buttons, glyph inset from the left edge) and draw_icon_centered
   (icon-only buttons) compute their own offsets before calling it. NULL
   icon_font (font failed to load) is a silent no-op. */
static void draw_icon_left(struct nk_context *ctx, struct nk_rect bounds, float inset_x,
                            unsigned int codepoint, const struct nk_user_font *icon_font,
                            struct nk_color color) {
    if (!icon_font) return;
    char buf[4];
    int len = icon_utf8(buf, codepoint);
    struct nk_rect r = bounds;
    r.x += inset_x;
    r.y += (bounds.h - icon_font->height) / 2.0f;
    nk_draw_text(nk_window_get_canvas(ctx), r, buf, len, icon_font, nk_rgba(0, 0, 0, 0), color);
}

static void draw_icon_centered(struct nk_context *ctx, struct nk_rect bounds, unsigned int codepoint,
                                const struct nk_user_font *icon_font, struct nk_color color) {
    if (!icon_font) return;
    char buf[4];
    int len = icon_utf8(buf, codepoint);
    float text_w = icon_font->width(icon_font->userdata, icon_font->height, buf, len);
    struct nk_rect r = bounds;
    r.x += (bounds.w - text_w) / 2.0f;
    r.y += (bounds.h - icon_font->height) / 2.0f;
    nk_draw_text(nk_window_get_canvas(ctx), r, buf, len, icon_font, nk_rgba(0, 0, 0, 0), color);
}

/* An icon+label button: draws as a normal nk_button_label (so hover/press/
   click all work exactly as usual), then overlays an icon glyph near its
   left edge. `label` should have a few leading spaces baked in (see call
   sites) so the button's own centered text doesn't sit under the icon. */
static int icon_button(struct nk_context *ctx, const struct nk_user_font *icon_font,
                        unsigned int codepoint, const char *label, struct nk_color icon_color) {
    struct nk_rect bounds = nk_widget_bounds(ctx);
    int clicked = nk_button_label(ctx, label);
    draw_icon_left(ctx, bounds, 8.0f, codepoint, icon_font, icon_color);
    return clicked;
}

/* Icon-only button (no label at all) - the sidebar collapse toggle. */
static int icon_only_button(struct nk_context *ctx, const struct nk_user_font *icon_font,
                             unsigned int codepoint, struct nk_color icon_color) {
    struct nk_rect bounds = nk_widget_bounds(ctx);
    int clicked = nk_button_label(ctx, "");
    draw_icon_centered(ctx, bounds, codepoint, icon_font, icon_color);
    return clicked;
}

/* Draws one header row + N data rows from a RowPreview into whatever group
   is currently open - shared by the "Registros" table tab and the SQL
   panel's result viewer so both grids look and behave identically. */
static void draw_row_grid(struct nk_context *ctx, const RowPreview *rp, int dark) {
    if (rp && rp->col_count > 0) {
        nk_layout_row_dynamic(ctx, 20, rp->col_count);
        for (int c = 0; c < rp->col_count; c++)
            nk_label_colored(ctx, rp->col_names[c], NK_TEXT_LEFT, accent_text_color(dark));
        for (int r = 0; r < rp->row_count; r++) {
            nk_layout_row_dynamic(ctx, 20, rp->col_count);
            for (int c = 0; c < rp->col_count; c++) nk_label(ctx, rp->cells[r][c], NK_TEXT_LEFT);
        }
    } else {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "(sin registros)", NK_TEXT_LEFT);
    }
}

/* Draws a text-edit field that supports Tab-to-next-field navigation,
   ghost placeholder text when empty, and (optionally) asterisk-masked
   display for passwords, all on top of the plain Nuklear edit widget. */
static nk_flags edit_field(struct nk_context *ctx, AppState *st, int *counter, nk_flags flags,
                            char *buf, int max, nk_plugin_filter filter, const char *placeholder,
                            int is_password) {
    int field_id = (*counter)++;
    if (st->pending_focus == field_id) {
        nk_edit_focus(ctx, flags);
        st->pending_focus = -1;
    }

    struct nk_rect bounds = nk_widget_bounds(ctx);
    nk_flags ret = nk_edit_string_zero_terminated(ctx, flags, buf, max, filter);
    if (ret & NK_EDIT_ACTIVE) st->focused_field = field_id;

    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect text_bounds = bounds;
    text_bounds.x += 4;
    text_bounds.w = (text_bounds.w > 8) ? text_bounds.w - 8 : text_bounds.w;

    if (buf[0] == '\0') {
        if (placeholder) {
            nk_draw_text(canvas, text_bounds, placeholder, (int)strlen(placeholder), ctx->style.font,
                         nk_rgba(0, 0, 0, 0), muted_text_color(st->dark_theme));
        }
    } else if (is_password) {
        struct nk_color bg = (ret & NK_EDIT_ACTIVE) ? ctx->style.edit.active.data.color : ctx->style.edit.normal.data.color;
        nk_fill_rect(canvas, bounds, ctx->style.edit.rounding, bg);
        size_t len = strlen(buf);
        char mask[128];
        if (len > sizeof(mask) - 1) len = sizeof(mask) - 1;
        memset(mask, '*', len);
        mask[len] = '\0';
        nk_draw_text(canvas, text_bounds, mask, (int)len, ctx->style.font, nk_rgba(0, 0, 0, 0),
                     ctx->style.edit.text_normal);
    }
    return ret;
}

/* Draws one row of a VS Code Explorer-style tree: indented by `depth`
   levels, no button chrome (just the selectable's own transparent/hover/
   accent state layer from apply_flat_metrics), highlighted while
   `selected`. Returns nonzero the frame it's clicked. */
static int tree_row(struct nk_context *ctx, const char *label, int depth, int selected) {
    nk_layout_row_template_begin(ctx, 24.0f);
    if (depth > 0) nk_layout_row_template_push_static(ctx, 14.0f * (float)depth);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);

    if (depth > 0) nk_spacing(ctx, 1);
    nk_bool value = (nk_bool)selected;
    return nk_selectable_label(ctx, label, NK_TEXT_LEFT, &value);
}

/* Like tree_row, but with a small trash-icon button appended - used for
   saved-connection rows in the sidebar, where deleting an entry is a
   distinct action from selecting it. Reports the two clicks separately via
   out-params rather than a single return value. */
static void tree_row_with_delete(struct nk_context *ctx, AppState *st, const char *label, int depth,
                                  int selected, int *clicked_row, int *clicked_delete) {
    nk_layout_row_template_begin(ctx, 24.0f);
    if (depth > 0) nk_layout_row_template_push_static(ctx, 14.0f * (float)depth);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 22.0f);
    nk_layout_row_template_end(ctx);

    if (depth > 0) nk_spacing(ctx, 1);
    nk_bool value = (nk_bool)selected;
    *clicked_row = nk_selectable_label(ctx, label, NK_TEXT_LEFT, &value);
    *clicked_delete = icon_only_button(ctx, st->icon_font, ICON_DELETE, muted_text_color(st->dark_theme));
}

static void clear_selected_table(AppState *st) {
    if (st->schema_valid) {
        db_table_free(&st->selected_schema);
        st->schema_valid = 0;
    }
    st->selected_table[0] = '\0';
    st->table_view_mode = 0;
    st->preview_offset = 0;
    st->preview_total_rows = -1;
    st->table_result_msg[0] = '\0';
}

static void disconnect_current(AppState *st) {
    if (st->backend && st->conn) st->backend->disconnect(st->conn);
    st->conn = NULL;
    st->connected = 0;
    st->picking_db = 0;
    st->table_count = 0;
    st->database_count = 0;
    st->active_database[0] = '\0';
    clear_selected_table(st);
    st->confirm_wipe = 0;
}

static void sql_editor_clear(AppState *st);

/* "Cerrar conexion": tears everything down and returns to the login form. */
static void close_connection(AppState *st) {
    disconnect_current(st);
    st->result_log[0] = '\0';
    st->connection_summary[0] = '\0';
    sql_editor_clear(st);
    st->sql_status_msg[0] = '\0';
    snprintf(st->status_msg, sizeof(st->status_msg), "%s", "Sin conexion.");
}

static void fill_params_from_form(AppState *st, DbConnParams *params) {
    memset(params, 0, sizeof(*params));
    snprintf(params->path, sizeof(params->path), "%s", st->sqlite_path);
    snprintf(params->host, sizeof(params->host), "%s", st->host);
    params->port = atoi(st->port_str);
    snprintf(params->user, sizeof(params->user), "%s", st->user);
    snprintf(params->password, sizeof(params->password), "%s", st->password);
}

/* Connects to a specific database and lists its tables; used both for
   SQLite (which has no server/database-list step) and, once a database has
   been picked, for server engines. */
static void connect_and_list_tables(AppState *st, DbConnParams *params) {
    char err[MAX_ERROR_LEN];
    DbConn *conn = st->backend->connect(params, err, sizeof(err));
    if (!conn) {
        if (st->engine == DB_ENGINE_SQLITE) {
            char cwd[256];
            if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), "?");
            snprintf(st->status_msg, sizeof(st->status_msg),
                     "Error al abrir '%s' (directorio actual: %s): %s. Usa una ruta absoluta o corre el "
                     "programa desde la carpeta del proyecto.",
                     st->sqlite_path, cwd, err);
        } else {
            snprintf(st->status_msg, sizeof(st->status_msg), "Error al conectar: %s", err);
        }
        return;
    }

    int n = st->backend->list_tables(conn, st->tables, MAX_TABLES);
    if (n < 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al listar tablas: %s", st->backend->last_error(conn));
        st->backend->disconnect(conn);
        return;
    }

    st->conn = conn;
    st->table_count = n;
    st->connected = 1;
    st->picking_db = 0;
    st->last_refresh_time = glfwGetTime();
    snprintf(st->status_msg, sizeof(st->status_msg), "Conectado (%d tabla%s).", n, n == 1 ? "" : "s");

    if (st->engine == DB_ENGINE_SQLITE) {
        snprintf(st->connection_summary, sizeof(st->connection_summary), "SQLite - %s", st->sqlite_path);
    } else {
        snprintf(st->connection_summary, sizeof(st->connection_summary), "%s - %s@%s:%s%s%s",
                 ENGINE_LABELS[st->engine], st->user, st->host, st->port_str,
                 params->database[0] ? " / " : "", params->database);
    }
}

static void do_connect(AppState *st) {
    disconnect_current(st);
    st->result_log[0] = '\0';

    st->backend = db_backend_for_engine(st->engine);
    if (!st->backend) {
        snprintf(st->status_msg, sizeof(st->status_msg),
                 "Backend '%s' aun no implementado (proxima entrega).", ENGINE_LABELS[st->engine]);
        return;
    }

    DbConnParams params;
    fill_params_from_form(st, &params);

    if (st->engine == DB_ENGINE_SQLITE || !st->backend->connect_server) {
        connect_and_list_tables(st, &params);
        return;
    }

    /* Server engine: connect without a database first, so the user can pick
       one from the list before we touch any table. */
    char err[MAX_ERROR_LEN];
    DbConn *conn = st->backend->connect_server(&params, err, sizeof(err));
    if (!conn) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al conectar al servidor: %s", err);
        return;
    }
    int n = st->backend->list_databases(conn, st->databases, MAX_TABLES);
    if (n < 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al listar bases: %s", st->backend->last_error(conn));
        st->backend->disconnect(conn);
        return;
    }
    st->conn = conn;
    st->database_count = n;
    st->picking_db = 1;
    snprintf(st->status_msg, sizeof(st->status_msg),
             "Conectado al servidor (%d base%s). Elige una para continuar.", n, n == 1 ? "" : "s");
}

/* Copies a saved profile's fields into the live connection form and
   connects - one level up from activate_database's accordion behavior:
   do_connect() already tears down any prior connection via
   disconnect_current(), so picking a different saved connection than the
   currently active one transparently reconnects. */
static void select_saved_connection(AppState *st, int index) {
    const SavedConnection *sc = &st->saved_connections[index];
    st->engine = sc->engine;
    snprintf(st->sqlite_path, sizeof(st->sqlite_path), "%s", sc->sqlite_path);
    snprintf(st->host, sizeof(st->host), "%s", sc->host);
    snprintf(st->port_str, sizeof(st->port_str), "%d", sc->port);
    snprintf(st->user, sizeof(st->user), "%s", sc->user);
    snprintf(st->password, sizeof(st->password), "%s", sc->password);
    st->active_saved_connection_index = index;
    st->view_mode = VIEW_CONNECTION;
    do_connect(st);
}

/* Saves the CURRENT connection form (filled in, connected or not) as a new
   named profile and rewrites the on-disk file immediately - there is no
   separate "unsaved changes" state to track. */
static void save_current_connection(AppState *st, const char *name) {
    if (st->saved_connection_count >= MAX_SAVED_CONNECTIONS) {
        snprintf(st->status_msg, sizeof(st->status_msg), "%s", "Ya hay demasiadas conexiones guardadas.");
        return;
    }
    SavedConnection *sc = &st->saved_connections[st->saved_connection_count];
    memset(sc, 0, sizeof(*sc));
    snprintf(sc->name, sizeof(sc->name), "%s", name);
    sc->engine = st->engine;
    snprintf(sc->sqlite_path, sizeof(sc->sqlite_path), "%s", st->sqlite_path);
    snprintf(sc->host, sizeof(sc->host), "%s", st->host);
    sc->port = atoi(st->port_str);
    snprintf(sc->user, sizeof(sc->user), "%s", st->user);
    snprintf(sc->password, sizeof(sc->password), "%s", st->password);
    st->saved_connection_count++;
    saved_connections_save(st->saved_connections, st->saved_connection_count);
}

/* Removes a saved connection (sidebar trash icon). If it was the active
   one, tears down the live connection too - staying "active" after its own
   profile vanishes from the list would be confusing. */
static void delete_saved_connection(AppState *st, int index) {
    if (index == st->active_saved_connection_index) {
        close_connection(st);
        st->active_saved_connection_index = -1;
        st->view_mode = VIEW_NEW_CONNECTION;
    } else if (index < st->active_saved_connection_index) {
        st->active_saved_connection_index--;
    }
    for (int i = index; i < st->saved_connection_count - 1; i++)
        st->saved_connections[i] = st->saved_connections[i + 1];
    st->saved_connection_count--;
    saved_connections_save(st->saved_connections, st->saved_connection_count);
}

/* Activates (connects to) a database from the sidebar. Accordion-style: since
   only one DbConn can be alive at a time, switching to a different database
   than the currently active one transparently reconnects. */
static void activate_database(AppState *st, const char *dbname) {
    if (strcmp(st->active_database, dbname) == 0 && st->connected) return; /* already active */

    if (st->backend && st->conn) st->backend->disconnect(st->conn);
    st->conn = NULL;
    st->picking_db = 0;
    clear_selected_table(st);

    DbConnParams params;
    fill_params_from_form(st, &params);
    snprintf(params.database, sizeof(params.database), "%s", dbname);
    connect_and_list_tables(st, &params);
    if (st->connected) snprintf(st->active_database, sizeof(st->active_database), "%s", dbname);
}

/* Closes (collapses) the currently active database - the other half of the
   toggle: clicking an already-open database in the sidebar used to be a
   no-op (activate_database's own guard bails out early since it's already
   active), so there was no way to close one short of disconnecting
   entirely. Drops the database-scoped connection and returns to the
   post-connect_server "pick a database" state, keeping the already-loaded
   database list so the sidebar doesn't lose its place. */
static void close_database(AppState *st) {
    if (st->backend && st->conn) st->backend->disconnect(st->conn);
    st->conn = NULL;
    st->connected = 0;
    st->picking_db = 1;
    st->table_count = 0;
    st->active_database[0] = '\0';
    clear_selected_table(st);
    st->confirm_wipe = 0;
}

static void select_table(AppState *st, const char *table_name);

static int has_suffix_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

static void refresh_browser(AppState *st) {
    st->browse_dir_count = 0;
    st->browse_file_count = 0;
    DIR *d = opendir(st->browse_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0) continue;
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", st->browse_dir, ent->d_name);
        struct stat sb;
        if (stat(full, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            if (st->browse_dir_count < 128)
                snprintf(st->browse_dirs[st->browse_dir_count++], MAX_NAME_LEN, "%s", ent->d_name);
        } else if (has_suffix_ci(ent->d_name, ".db") || has_suffix_ci(ent->d_name, ".sqlite") ||
                   has_suffix_ci(ent->d_name, ".sqlite3")) {
            if (st->browse_file_count < 128)
                snprintf(st->browse_files[st->browse_file_count++], MAX_NAME_LEN, "%s", ent->d_name);
        }
    }
    closedir(d);
}

static void open_browser(AppState *st) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", st->sqlite_path);
    char *slash = strrchr(tmp, '/');
    if (slash) { *slash = '\0'; snprintf(st->browse_dir, sizeof(st->browse_dir), "%s", tmp[0] ? tmp : "/"); }
    else snprintf(st->browse_dir, sizeof(st->browse_dir), ".");
    st->show_browser = 1;
    refresh_browser(st);
}

/* "Examinar..." button: tries the OS's own native file picker first (real
   desktop integration, both platforms - zenity/kdialog on Linux, the Win32
   common dialog on Windows), and only falls back to dbfiller's in-app
   browser above if no native dialog is available. */
static void pick_sqlite_file(AppState *st) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", st->sqlite_path);
    char *slash = strrchr(tmp, '/');
    if (slash) *slash = '\0';
    const char *initial_dir = slash ? (tmp[0] ? tmp : "/") : ".";

    char picked[512];
    if (native_open_file_dialog(picked, sizeof(picked), initial_dir)) {
        snprintf(st->sqlite_path, sizeof(st->sqlite_path), "%s", picked);
        return;
    }
    open_browser(st);
}

static void log_append(AppState *st, const char *fmt, ...) {
    size_t used = strlen(st->result_log);
    if (used >= sizeof(st->result_log) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->result_log + used, sizeof(st->result_log) - used, fmt, ap);
    va_end(ap);
}

static void do_fill_all(AppState *st) {
    if (!st->connected || st->table_count == 0) return;
    st->result_log[0] = '\0';

    int count = atoi(st->count_str);
    if (count <= 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Cantidad invalida.");
        return;
    }

    char err[MAX_ERROR_LEN];
    OrderedTables ordered;
    if (db_graph_load_ordered(st->backend, st->conn, st->tables, st->table_count, &ordered, err, sizeof(err)) != 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al leer esquema: %s", err);
        return;
    }

    int total_ins = 0, total_fail = 0;
    for (int i = 0; i < ordered.count; i++) {
        const DbTable *table = &ordered.tables[i];
        GenerateStats stats;
        generate_and_insert(st->backend, st->conn, table, count, &stats);
        log_append(st, "%s: insertados %d, fallidos %d\n", table->name, stats.rows_inserted, stats.rows_failed);
        if (stats.rows_failed > 0) log_append(st, "   ultimo error: %s\n", stats.last_error);
        total_ins += stats.rows_inserted;
        total_fail += stats.rows_failed;
    }
    log_append(st, "\nTotal: %d insertados, %d fallidos en %d tabla(s).", total_ins, total_fail, ordered.count);
    snprintf(st->status_msg, sizeof(st->status_msg), "Listo: %d insertados, %d fallidos.", total_ins, total_fail);

    db_graph_free(&ordered);
}

/* "Vaciar base de datos": deletes every row from every table, in
   child-before-parent order (the reverse of db_graph_load_ordered's
   parent-first fill order) so foreign keys don't reject the delete. Wrapped
   in one transaction so a failure partway through leaves the database
   untouched instead of half-emptied. Called only after the user confirms
   (see st->confirm_wipe in draw_ui) - there is no undo for this. */
static void do_wipe_database(AppState *st) {
    if (!st->connected || st->table_count == 0) return;
    st->result_log[0] = '\0';

    char err[MAX_ERROR_LEN];
    OrderedTables ordered;
    if (db_graph_load_ordered(st->backend, st->conn, st->tables, st->table_count, &ordered, err, sizeof(err)) != 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al leer esquema: %s", err);
        return;
    }

    if (st->backend->begin_tx(st->conn) != 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al iniciar transaccion: %s",
                 st->backend->last_error(st->conn));
        db_graph_free(&ordered);
        return;
    }

    int wiped = 0, failed = 0;
    for (int i = ordered.count - 1; i >= 0; i--) {
        const DbTable *table = &ordered.tables[i];
        if (st->backend->delete_all_rows(st->conn, table->name, err, sizeof(err)) != 0) {
            log_append(st, "%s: error al vaciar - %s\n", table->name, err);
            failed++;
        } else {
            log_append(st, "%s: vaciada\n", table->name);
            wiped++;
        }
    }

    if (failed > 0) {
        st->backend->rollback_tx(st->conn);
        log_append(st, "\nSe revirtio todo por %d error(es); la base de datos no cambio.", failed);
        snprintf(st->status_msg, sizeof(st->status_msg), "Error al vaciar: %d tabla(s) fallaron, se revirtio todo.", failed);
    } else {
        st->backend->commit_tx(st->conn);
        log_append(st, "\nTotal: %d tabla(s) vaciadas.", wiped);
        snprintf(st->status_msg, sizeof(st->status_msg), "Listo: %d tabla(s) vaciadas.", wiped);
    }

    db_graph_free(&ordered);
}

static void load_row_preview(AppState *st) {
    if (!st->row_preview || st->selected_table[0] == '\0') return;
    char err[MAX_ERROR_LEN];
    if (st->backend->fetch_rows(st->conn, st->selected_table, st->preview_offset, MAX_PREVIEW_ROWS,
                                 st->row_preview, err, sizeof(err)) != 0) {
        memset(st->row_preview, 0, sizeof(*st->row_preview));
        snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Error al leer registros: %s", err);
    }
    long long total = -1;
    if (st->backend->count_rows(st->conn, st->selected_table, &total, err, sizeof(err)) != 0)
        total = -1;
    st->preview_total_rows = total;
}

static void select_table(AppState *st, const char *table_name) {
    clear_selected_table(st);
    snprintf(st->selected_table, sizeof(st->selected_table), "%s", table_name);

    if (st->backend->get_table_schema(st->conn, table_name, &st->selected_schema) != 0) {
        snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Error al leer esquema: %s",
                 st->backend->last_error(st->conn));
        st->selected_table[0] = '\0';
        return;
    }
    st->schema_valid = 1;
    load_row_preview(st);
}

/* Re-reads the schema/rows of whatever table is currently selected, keeping
   the current view mode and page offset (unlike select_table, which resets
   both) so a refresh doesn't yank the user back to the top of the grid. */
static void refresh_selected_table_schema(AppState *st) {
    if (st->selected_table[0] == '\0') return;

    if (st->schema_valid) {
        db_table_free(&st->selected_schema);
        st->schema_valid = 0;
    }
    if (st->backend->get_table_schema(st->conn, st->selected_table, &st->selected_schema) != 0) {
        snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Error al leer esquema: %s",
                 st->backend->last_error(st->conn));
        st->selected_table[0] = '\0';
        return;
    }
    st->schema_valid = 1;
    if (st->table_view_mode == 1) load_row_preview(st);
}

/* Re-lists tables (and, if one is selected, its schema/rows) against the
   live connection, so changes made outside the app - e.g. a CREATE TABLE run
   from a terminal - show up without a manual disconnect/reconnect. Errors
   are only surfaced to status_msg when triggered manually (silent == 0),
   since the automatic timer in main() runs this quietly every couple of
   seconds and shouldn't spam the status line. */
static void refresh_current(AppState *st, int silent) {
    if (!st->connected || !st->backend || !st->conn) return;

    int n = st->backend->list_tables(st->conn, st->tables, MAX_TABLES);
    if (n < 0) {
        if (!silent) {
            snprintf(st->status_msg, sizeof(st->status_msg), "Error al refrescar: %s",
                     st->backend->last_error(st->conn));
        }
        return;
    }
    st->table_count = n;

    if (st->selected_table[0]) {
        int still_exists = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(st->tables[i], st->selected_table) == 0) { still_exists = 1; break; }
        }
        if (!still_exists) clear_selected_table(st);
        else refresh_selected_table_schema(st);
    }

    if (!silent) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Actualizado (%d tabla%s).", n, n == 1 ? "" : "s");
    }
}

/* Generates records for just the selected table (the assignment's core
   single-table flow), as opposed to do_fill_all() which sweeps every table
   in the database. */
static void do_generate_single_table(AppState *st) {
    if (st->selected_table[0] == '\0') return;
    int count = atoi(st->single_count_str);
    if (count <= 0) {
        snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Cantidad invalida.");
        return;
    }

    char err[MAX_ERROR_LEN];
    OrderedTables ordered;
    char names[1][MAX_NAME_LEN];
    snprintf(names[0], MAX_NAME_LEN, "%s", st->selected_table);
    if (db_graph_load_ordered(st->backend, st->conn, names, 1, &ordered, err, sizeof(err)) != 0) {
        snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Error al leer esquema: %s", err);
        return;
    }

    GenerateStats stats;
    generate_and_insert(st->backend, st->conn, &ordered.tables[0], count, &stats);
    snprintf(st->table_result_msg, sizeof(st->table_result_msg), "Insertados: %d, fallidos: %d%s%s",
             stats.rows_inserted, stats.rows_failed,
             stats.rows_failed > 0 ? " - " : "", stats.rows_failed > 0 ? stats.last_error : "");
    db_graph_free(&ordered);

    if (st->table_view_mode == 1) load_row_preview(st);
}

/* Copies the SQL editor's current content out of its nk_text_edit into a
   plain null-terminated string. sql_edit.string isn't null-terminated on
   its own (nk_str tracks length separately), so this is the only correct
   way to read it. */
static void sql_text_snapshot(AppState *st, char *out, size_t out_size) {
    const char *text = nk_str_get_const(&st->sql_edit.string);
    int len = nk_str_len_char(&st->sql_edit.string);
    if ((size_t)len >= out_size) len = (int)out_size - 1;
    memcpy(out, text, (size_t)len);
    out[len] = '\0';
}

/* Resets the SQL editor to empty, including the text_edit's own cursor/
   selection state (nk_str_clear alone only resets the string length). */
static void sql_editor_clear(AppState *st) {
    nk_str_clear(&st->sql_edit.string);
    st->sql_edit.cursor = 0;
    st->sql_edit.select_start = 0;
    st->sql_edit.select_end = 0;
    st->sql_word_start = 0;
    st->sql_word_len = 0;
    st->sql_suggest_count = 0;
    st->sql_suggest_index = 0;
    st->sql_base_query[0] = '\0';
    st->sql_offset = 0;
    st->sql_pageable = 0;
    st->sql_total_rows = -1;
}

/* Walks backward from `cursor` over identifier characters to find the start
   of the word currently being typed, e.g. cursor right after "FR" in
   "SELECT * FR" finds word_start pointing at the 'F'. */
static int sql_current_word(const char *text, int cursor, int *out_start) {
    int start = cursor;
    while (start > 0) {
        char c = text[start - 1];
        if (!(isalnum((unsigned char)c) || c == '_')) break;
        start--;
    }
    *out_start = start;
    return cursor - start;
}

/* Pixel offset of `cursor` (byte index into `text`) relative to the edit
   widget's text area origin - i.e. how far right on its own line, and how
   far down from the first line. Mirrors the cursor_pos calculation
   nk_do_edit() does internally (nuklear.h) so the floating suggestion list
   can be anchored to the line the caret is actually on, instead of the
   bottom of the whole multi-line box. */
static struct nk_vec2 sql_cursor_line_offset(struct nk_context *ctx, const char *text, int cursor, float row_height) {
    const struct nk_user_font *font = ctx->style.font;
    int line_start = 0;
    float y = 0.0f;
    for (int i = 0; i < cursor; i++) {
        if (text[i] == '\n') {
            y += row_height;
            line_start = i + 1;
        }
    }
    float x = font->width(font->userdata, font->height, text + line_start, cursor - line_start);
    return nk_vec2(x, y);
}

/* ---- Syntax-aware autocomplete for the SQL tab -------------------------
   Not a real SQL parser: a lightweight tokenizer plus a single forward scan
   that tracks "the nearest preceding clause keyword" and "which tables/
   aliases have been referenced so far". That's enough to predict a
   plausible next token (suggest table names after FROM/JOIN, columns after
   SELECT/WHERE/ON/..., and narrow to one table's columns after
   "alias."/"table.") without needing to understand the whole statement. */

typedef enum {
    SQL_CTX_STATEMENT_START, /* start of input, or right after ';' */
    SQL_CTX_SELECT_LIST,     /* between SELECT/DISTINCT and FROM */
    SQL_CTX_TABLE_EXPECTED,  /* right after FROM/JOIN/INTO/UPDATE/TABLE/',' in a FROM-list */
    SQL_CTX_AFTER_TABLE,        /* just finished "FROM t"/"FROM t alias" or "JOIN t" */
    SQL_CTX_AFTER_INSERT_TABLE, /* just finished "INSERT INTO t" - WHERE/JOIN/etc make no sense here */
    SQL_CTX_AFTER_UPDATE_TABLE, /* just finished "UPDATE t" - only SET makes sense next */
    SQL_CTX_CONDITION,       /* WHERE/AND/OR/HAVING/ON */
    SQL_CTX_COLUMN_LIST,     /* GROUP BY / ORDER BY */
    SQL_CTX_ASSIGNMENT,      /* UPDATE ... SET */
    SQL_CTX_GENERIC          /* sql_ctx_for_keyword's "not a context-changing
                                 token" sentinel; also (re)used as an actual
                                 stored state for "just finished CREATE/DROP/
                                 ALTER TABLE t" - no further continuation is
                                 modeled there, so "suggest nothing" (this
                                 context's default behavior) is the correct,
                                 safe answer rather than a dedicated context. */
} SqlContext;

#define SQL_MAX_ALIASES 16
typedef struct {
    char alias[MAX_NAME_LEN]; /* == table when referenced without an alias */
    char table[MAX_NAME_LEN];
} SqlAliasBinding;

typedef struct {
    SqlContext ctx;
    int has_qualifier;
    char qualifier[MAX_NAME_LEN]; /* set when text ends in "ident." right before the word being typed */
    SqlAliasBinding aliases[SQL_MAX_ALIASES];
    int alias_count;
} SqlParseContext;

typedef struct { int start, len; } SqlTok;

/* Splits text[0..upto) into identifier and single-char punctuation ('.', ',',
   ';') tokens, skipping whitespace, '..'/".." quoted strings and '--' line
   comments. Capped at max_toks - typed SQL is short, so truncating a
   pathological statement just makes context detection a bit less precise,
   never unsafe. */
static int sql_tokenize(const char *text, int upto, SqlTok *out, int max_toks) {
    int n = 0, i = 0;
    while (i < upto && n < max_toks) {
        char c = text[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '\'' || c == '"') {
            char q = c;
            i++;
            while (i < upto && text[i] != q) i++;
            if (i < upto) i++;
            continue;
        }
        if (c == '-' && i + 1 < upto && text[i + 1] == '-') {
            while (i < upto && text[i] != '\n') i++;
            continue;
        }
        if (isalnum((unsigned char)c) || c == '_') {
            int s = i;
            while (i < upto && (isalnum((unsigned char)text[i]) || text[i] == '_')) i++;
            out[n].start = s; out[n].len = i - s; n++;
            continue;
        }
        if (c == '.' || c == ',' || c == ';') {
            out[n].start = i; out[n].len = 1; n++;
            i++;
            continue;
        }
        i++;
    }
    return n;
}

static int sql_tok_eq(const char *text, SqlTok t, const char *kw) {
    int klen = (int)strlen(kw);
    return t.len == klen && strncasecmp(text + t.start, kw, (size_t)klen) == 0;
}

static int sql_tok_is_punct(const char *text, SqlTok t, char c) {
    return t.len == 1 && text[t.start] == c;
}

/* Maps a single token to the context it introduces, or SQL_CTX_GENERIC if
   it's not one of the clause keywords that changes context (the sentinel
   is never itself stored as a context - see sql_analyze_context). */
static SqlContext sql_ctx_for_keyword(const char *text, SqlTok t) {
    static const struct { const char *kw; SqlContext ctx; } MARKERS[] = {
        {"SELECT", SQL_CTX_SELECT_LIST}, {"DISTINCT", SQL_CTX_SELECT_LIST},
        {"FROM", SQL_CTX_TABLE_EXPECTED}, {"JOIN", SQL_CTX_TABLE_EXPECTED},
        {"INTO", SQL_CTX_TABLE_EXPECTED}, {"UPDATE", SQL_CTX_TABLE_EXPECTED},
        {"TABLE", SQL_CTX_TABLE_EXPECTED},
        {"WHERE", SQL_CTX_CONDITION}, {"AND", SQL_CTX_CONDITION},
        {"OR", SQL_CTX_CONDITION}, {"HAVING", SQL_CTX_CONDITION}, {"ON", SQL_CTX_CONDITION},
        {"GROUP", SQL_CTX_COLUMN_LIST}, {"ORDER", SQL_CTX_COLUMN_LIST},
        {"SET", SQL_CTX_ASSIGNMENT},
    };
    for (size_t i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++)
        if (sql_tok_eq(text, t, MARKERS[i].kw)) return MARKERS[i].ctx;
    return SQL_CTX_GENERIC;
}

/* Which statement verb put us in SQL_CTX_TABLE_EXPECTED - determines which
   context to switch to once the table name itself has been consumed (see
   sql_analyze_context), since "FROM t", "INSERT INTO t" and "UPDATE t" all
   expect a table name next but have completely different valid
   continuations. */
typedef enum { SQL_TBL_ROLE_FROM, SQL_TBL_ROLE_INSERT, SQL_TBL_ROLE_UPDATE, SQL_TBL_ROLE_DDL } SqlTableRole;

static SqlTableRole sql_table_role_for_keyword(const char *text, SqlTok t) {
    if (sql_tok_eq(text, t, "INTO")) return SQL_TBL_ROLE_INSERT;
    if (sql_tok_eq(text, t, "UPDATE")) return SQL_TBL_ROLE_UPDATE;
    if (sql_tok_eq(text, t, "TABLE")) return SQL_TBL_ROLE_DDL;
    return SQL_TBL_ROLE_FROM; /* FROM, JOIN (and any of its variants) */
}

static int sql_tok_is_reserved(const char *text, SqlTok t) {
    for (int i = 0; i < SQL_RESERVED_TOKENS_COUNT; i++)
        if (sql_tok_eq(text, t, SQL_RESERVED_TOKENS[i])) return 1;
    return 0;
}

/* Scans text[0..upto) (upto is where the word currently being typed starts,
   so that in-progress word is never itself part of the context) tracking
   the nearest preceding clause keyword and every "<table> [AS] <alias>"
   binding seen after FROM/JOIN/UPDATE/INTO. ';' resets both, so pasting
   several statements at once doesn't leak context across them. Deliberately
   loose about subqueries/CTEs - this drives predictive typing, not
   validation, so a slightly-wrong guess just means a slightly-less-sharp
   suggestion list, never a wrong query. */
static void sql_analyze_context(const char *text, int upto, SqlParseContext *out) {
    memset(out, 0, sizeof(*out));
    out->ctx = SQL_CTX_STATEMENT_START;

    SqlTok toks[512];
    int n = sql_tokenize(text, upto, toks, 512);

    int expecting_table = 0;
    SqlTableRole table_role = SQL_TBL_ROLE_FROM;
    char pending_table[MAX_NAME_LEN] = {0};
    int have_pending_table = 0;

    for (int i = 0; i < n; i++) {
        SqlTok t = toks[i];

        if (sql_tok_is_punct(text, t, ';')) {
            out->ctx = SQL_CTX_STATEMENT_START;
            out->alias_count = 0;
            expecting_table = 0;
            have_pending_table = 0;
            continue;
        }
        if (sql_tok_is_punct(text, t, '.')) continue;
        if (sql_tok_is_punct(text, t, ',')) {
            /* Old-style "FROM a, b" comma joins only make sense for an
               actual FROM-list, never after "INSERT INTO t," or
               "UPDATE t," - those aren't valid SQL, so table_role must
               already be FROM here (set right below when the table name
               was consumed). */
            if (out->ctx == SQL_CTX_TABLE_EXPECTED ||
                (out->ctx == SQL_CTX_AFTER_TABLE && table_role == SQL_TBL_ROLE_FROM)) {
                out->ctx = SQL_CTX_TABLE_EXPECTED;
                expecting_table = 1;
                table_role = SQL_TBL_ROLE_FROM;
            }
            have_pending_table = 0;
            continue;
        }

        SqlContext marker = sql_ctx_for_keyword(text, t);
        if (marker != SQL_CTX_GENERIC) {
            out->ctx = marker;
            expecting_table = (marker == SQL_CTX_TABLE_EXPECTED);
            if (expecting_table) table_role = sql_table_role_for_keyword(text, t);
            have_pending_table = 0;
            continue;
        }
        if (sql_tok_is_reserved(text, t)) {
            /* "AS" is the one reserved word that doesn't end a pending
               table's alias window - it just says "the alias comes next". */
            if (!sql_tok_eq(text, t, "AS")) have_pending_table = 0;
            continue;
        }

        /* A real identifier. */
        if (expecting_table) {
            snprintf(pending_table, sizeof(pending_table), "%.*s", t.len, text + t.start);
            if (out->alias_count < SQL_MAX_ALIASES) {
                snprintf(out->aliases[out->alias_count].alias, MAX_NAME_LEN, "%s", pending_table);
                snprintf(out->aliases[out->alias_count].table, MAX_NAME_LEN, "%s", pending_table);
                out->alias_count++;
            }
            expecting_table = 0;
            have_pending_table = 1;
            switch (table_role) {
                case SQL_TBL_ROLE_INSERT: out->ctx = SQL_CTX_AFTER_INSERT_TABLE; break;
                case SQL_TBL_ROLE_UPDATE: out->ctx = SQL_CTX_AFTER_UPDATE_TABLE; break;
                case SQL_TBL_ROLE_DDL:    out->ctx = SQL_CTX_GENERIC; break;
                default:                  out->ctx = SQL_CTX_AFTER_TABLE; break;
            }
        } else if (have_pending_table) {
            if (out->alias_count < SQL_MAX_ALIASES) {
                snprintf(out->aliases[out->alias_count].alias, MAX_NAME_LEN, "%.*s", t.len, text + t.start);
                snprintf(out->aliases[out->alias_count].table, MAX_NAME_LEN, "%s", pending_table);
                out->alias_count++;
            }
            have_pending_table = 0;
        }
    }

    if (n > 1 && sql_tok_is_punct(text, toks[n - 1], '.')) {
        SqlTok prev = toks[n - 2];
        if (prev.len > 0 && prev.len < MAX_NAME_LEN) {
            snprintf(out->qualifier, MAX_NAME_LEN, "%.*s", prev.len, text + prev.start);
            out->has_qualifier = 1;
        }
    }
}

static SqlSchemaCache *sql_schema_cache_find(AppState *st, const char *table_name) {
    for (int i = 0; i < st->sql_schema_cache_count; i++)
        if (strcasecmp(st->sql_schema_cache[i].name, table_name) == 0) return &st->sql_schema_cache[i];
    return NULL;
}

/* Resolves a "qualifier." prefix (from SqlParseContext.qualifier) to a real
   table name: first against aliases bound in this statement (most recent
   wins, e.g. self-joins), then against real table names directly (covers
   "users.id" with no alias at all). */
static int sql_resolve_qualifier(AppState *st, const SqlParseContext *pc, const char *qualifier,
                                  char *out_table, size_t out_size) {
    for (int i = pc->alias_count - 1; i >= 0; i--) {
        if (strcasecmp(pc->aliases[i].alias, qualifier) == 0) {
            snprintf(out_table, out_size, "%s", pc->aliases[i].table);
            return 1;
        }
    }
    for (int i = 0; i < st->table_count; i++) {
        if (strcasecmp(st->tables[i], qualifier) == 0) {
            snprintf(out_table, out_size, "%s", st->tables[i]);
            return 1;
        }
    }
    return 0;
}

/* Rebuilds the per-table column-name cache used by SQL-tab autocomplete
   from the connection's current st->tables[] list - one get_table_schema
   round-trip per table, so this is called on specific triggers (opening the
   SQL tab, after a DDL/DML statement runs from it) rather than every frame
   or on the periodic background table-list refresh. */
static void sql_refresh_schema_cache(AppState *st) {
    if (!st->sql_schema_cache || !st->connected) return;
    int count = st->table_count;
    if (count > MAX_TABLES) count = MAX_TABLES;
    for (int i = 0; i < count; i++) {
        SqlSchemaCache *entry = &st->sql_schema_cache[i];
        snprintf(entry->name, MAX_NAME_LEN, "%s", st->tables[i]);
        DbTable table;
        memset(&table, 0, sizeof(table));
        if (st->backend->get_table_schema(st->conn, st->tables[i], &table) == 0) {
            int cc = table.column_count;
            if (cc > SQL_SCHEMA_CACHE_MAX_COLUMNS) cc = SQL_SCHEMA_CACHE_MAX_COLUMNS;
            for (int c = 0; c < cc; c++)
                snprintf(entry->columns[c], MAX_NAME_LEN, "%s", table.columns[c].name);
            entry->column_count = cc;
            db_table_free(&table);
        } else {
            entry->column_count = 0;
        }
    }
    st->sql_schema_cache_count = count;
}

static int sql_suggest_matches(const char *candidate, const char *word, int word_len) {
    return (int)strlen(candidate) > word_len &&
           (word_len == 0 || strncasecmp(candidate, word, (size_t)word_len) == 0);
}

static int sql_suggest_has(SqlSuggestion *out, int n, const char *text) {
    for (int i = 0; i < n; i++)
        if (strcasecmp(out[i].text, text) == 0) return 1;
    return 0;
}

static void sql_suggest_add(SqlSuggestion *out, int *n, const char *text, SqlSuggestKind kind,
                             const char *word, int word_len) {
    if (*n >= SQL_SUGGEST_MAX || !sql_suggest_matches(text, word, word_len) || sql_suggest_has(out, *n, text))
        return;
    out[*n].text = text;
    out[*n].kind = kind;
    (*n)++;
}

/* Builds the floating suggestion list for the word starting at word_start
   (word_len bytes long, possibly 0 - see below). Layering:
     1. Context-specific candidates first (from sql_analyze_context): table
        names after FROM/JOIN, columns after SELECT/WHERE/ON/SET/GROUP BY/
        ORDER BY (scoped to just the tables referenced so far when any are
        known), or - exclusively - one table's columns after "x.".
     2. If the user is actively typing a prefix (word_len > 0), broaden to
        every keyword/table/column so a wrong context guess never hides a
        real completion. Skipped for an empty word (cursor just parked
        after a space/newline), where flooding a "what's next" popup with
        the entire keyword universe would defeat the point of it being
        predictive. */
static int sql_collect_suggestions(AppState *st, const char *text, int word_start, int word_len,
                                    SqlSuggestion out[SQL_SUGGEST_MAX]) {
    const char *word = text + word_start;
    int n = 0;

    SqlParseContext pc;
    sql_analyze_context(text, word_start, &pc);

    if (pc.has_qualifier) {
        char table_name[MAX_NAME_LEN];
        if (sql_resolve_qualifier(st, &pc, pc.qualifier, table_name, sizeof(table_name))) {
            SqlSchemaCache *tc = sql_schema_cache_find(st, table_name);
            if (tc) {
                for (int i = 0; i < tc->column_count && n < SQL_SUGGEST_MAX; i++)
                    sql_suggest_add(out, &n, tc->columns[i], SQL_SUG_COLUMN, word, word_len);
            }
        }
        return n; /* dot-completion is exclusive: typing "t.|" means "a column", never a keyword */
    }

    const char *scoped_tables[SQL_MAX_ALIASES];
    int scoped_count = 0;
    for (int i = 0; i < pc.alias_count; i++) {
        int dup = 0;
        for (int j = 0; j < scoped_count; j++)
            if (strcasecmp(scoped_tables[j], pc.aliases[i].table) == 0) { dup = 1; break; }
        if (!dup && scoped_count < SQL_MAX_ALIASES) scoped_tables[scoped_count++] = pc.aliases[i].table;
    }

    const char **primary_kw = NULL;
    int primary_kw_count = 0;
    const char **extra_kw = NULL;
    int extra_kw_count = 0;
    int suggest_tables = 0, suggest_columns = 0;
    /* WHERE/GROUP BY/ORDER BY/SET are all "a column comes next, almost
       always" contexts - putting columns ahead of the handful of keyword
       hints for those means the column list isn't crowded out of
       SQL_SUGGEST_MAX slots by keywords the user is far less likely to
       want right there. SELECT's column list is the opposite: FROM, the
       star, and DISTINCT are typically more useful than a bare column name
       right after SELECT (the table isn't even known yet), so keywords
       lead. */
    int columns_first = 0;
    switch (pc.ctx) {
        case SQL_CTX_STATEMENT_START:
            primary_kw = SQL_KW_STATEMENT_START;
            primary_kw_count = (int)(sizeof(SQL_KW_STATEMENT_START) / sizeof(*SQL_KW_STATEMENT_START));
            extra_kw = SQL_KW_STATEMENT_START_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_STATEMENT_START_EXTRA) / sizeof(*SQL_KW_STATEMENT_START_EXTRA));
            break;
        case SQL_CTX_SELECT_LIST:
            primary_kw = SQL_KW_SELECT_LIST;
            primary_kw_count = (int)(sizeof(SQL_KW_SELECT_LIST) / sizeof(*SQL_KW_SELECT_LIST));
            extra_kw = SQL_KW_SELECT_LIST_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_SELECT_LIST_EXTRA) / sizeof(*SQL_KW_SELECT_LIST_EXTRA));
            suggest_columns = 1;
            break;
        case SQL_CTX_TABLE_EXPECTED:
            /* Only a table (or subquery) is grammatically valid here - no
               keyword, column, AND/OR/etc. ever belongs right after FROM. */
            suggest_tables = 1;
            break;
        case SQL_CTX_AFTER_TABLE:
            primary_kw = SQL_KW_AFTER_TABLE;
            primary_kw_count = (int)(sizeof(SQL_KW_AFTER_TABLE) / sizeof(*SQL_KW_AFTER_TABLE));
            extra_kw = SQL_KW_AFTER_TABLE_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_AFTER_TABLE_EXTRA) / sizeof(*SQL_KW_AFTER_TABLE_EXTRA));
            suggest_tables = 1; /* old-style "FROM a, b" comma joins */
            break;
        case SQL_CTX_AFTER_INSERT_TABLE:
            primary_kw = SQL_KW_AFTER_INSERT_TABLE;
            primary_kw_count = (int)(sizeof(SQL_KW_AFTER_INSERT_TABLE) / sizeof(*SQL_KW_AFTER_INSERT_TABLE));
            break;
        case SQL_CTX_AFTER_UPDATE_TABLE:
            primary_kw = SQL_KW_AFTER_UPDATE_TABLE;
            primary_kw_count = (int)(sizeof(SQL_KW_AFTER_UPDATE_TABLE) / sizeof(*SQL_KW_AFTER_UPDATE_TABLE));
            break;
        case SQL_CTX_CONDITION:
            primary_kw = SQL_KW_CONDITION;
            primary_kw_count = (int)(sizeof(SQL_KW_CONDITION) / sizeof(*SQL_KW_CONDITION));
            extra_kw = SQL_KW_CONDITION_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_CONDITION_EXTRA) / sizeof(*SQL_KW_CONDITION_EXTRA));
            suggest_columns = 1;
            columns_first = 1;
            break;
        case SQL_CTX_COLUMN_LIST:
            primary_kw = SQL_KW_COLUMN_LIST;
            primary_kw_count = (int)(sizeof(SQL_KW_COLUMN_LIST) / sizeof(*SQL_KW_COLUMN_LIST));
            extra_kw = SQL_KW_COLUMN_LIST_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_COLUMN_LIST_EXTRA) / sizeof(*SQL_KW_COLUMN_LIST_EXTRA));
            suggest_columns = 1;
            columns_first = 1;
            break;
        case SQL_CTX_ASSIGNMENT:
            /* Right after SET (a column expected) or right after a column's
               "=" (a value expected) - WHERE is the one keyword worth
               hinting once a value's been typed; there's no useful primary
               keyword before that. */
            extra_kw = SQL_KW_ASSIGNMENT_EXTRA;
            extra_kw_count = (int)(sizeof(SQL_KW_ASSIGNMENT_EXTRA) / sizeof(*SQL_KW_ASSIGNMENT_EXTRA));
            suggest_columns = 1;
            columns_first = 1;
            break;
        default:
            break;
    }

    for (int pass = 0; pass < 2; pass++) {
        int do_columns = columns_first ? (pass == 0) : (pass == 1);
        if (do_columns) {
            if (!suggest_columns) continue;
            if (scoped_count > 0) {
                for (int s = 0; s < scoped_count && n < SQL_SUGGEST_MAX; s++) {
                    SqlSchemaCache *tc = sql_schema_cache_find(st, scoped_tables[s]);
                    if (!tc) continue;
                    for (int c = 0; c < tc->column_count && n < SQL_SUGGEST_MAX; c++)
                        sql_suggest_add(out, &n, tc->columns[c], SQL_SUG_COLUMN, word, word_len);
                }
            } else {
                for (int t = 0; t < st->sql_schema_cache_count && n < SQL_SUGGEST_MAX; t++)
                    for (int c = 0; c < st->sql_schema_cache[t].column_count && n < SQL_SUGGEST_MAX; c++)
                        sql_suggest_add(out, &n, st->sql_schema_cache[t].columns[c], SQL_SUG_COLUMN, word, word_len);
            }
        } else {
            for (int i = 0; i < primary_kw_count && n < SQL_SUGGEST_MAX; i++)
                sql_suggest_add(out, &n, primary_kw[i], SQL_SUG_KEYWORD, word, word_len);
            if (suggest_tables) {
                for (int i = 0; i < st->table_count && n < SQL_SUGGEST_MAX; i++)
                    sql_suggest_add(out, &n, st->tables[i], SQL_SUG_TABLE, word, word_len);
            }
        }
    }

    /* Broaden only within the CURRENT syntactic context - never to a global
       "every keyword that exists" list - and only while the user is
       actively typing a prefix (an empty word means the cursor just parked
       after a space, where flooding the popup with every remaining
       context-valid token would defeat the point of it being predictive).
       This is what keeps e.g. "CREATE TABLE" from ever surfacing while
       typing inside a WHERE clause: extra_kw only ever holds tokens valid
       in pc.ctx, tables are only added when suggest_tables is set for this
       context, and columns only when suggest_columns is set. */
    if (word_len > 0) {
        for (int i = 0; i < extra_kw_count && n < SQL_SUGGEST_MAX; i++)
            sql_suggest_add(out, &n, extra_kw[i], SQL_SUG_KEYWORD, word, word_len);
        if (suggest_tables) {
            for (int i = 0; i < st->table_count && n < SQL_SUGGEST_MAX; i++)
                sql_suggest_add(out, &n, st->tables[i], SQL_SUG_TABLE, word, word_len);
        }
        if (suggest_columns) {
            if (scoped_count > 0) {
                for (int s = 0; s < scoped_count && n < SQL_SUGGEST_MAX; s++) {
                    SqlSchemaCache *tc = sql_schema_cache_find(st, scoped_tables[s]);
                    if (!tc) continue;
                    for (int c = 0; c < tc->column_count && n < SQL_SUGGEST_MAX; c++)
                        sql_suggest_add(out, &n, tc->columns[c], SQL_SUG_COLUMN, word, word_len);
                }
            } else {
                for (int t = 0; t < st->sql_schema_cache_count && n < SQL_SUGGEST_MAX; t++)
                    for (int c = 0; c < st->sql_schema_cache[t].column_count && n < SQL_SUGGEST_MAX; c++)
                        sql_suggest_add(out, &n, st->sql_schema_cache[t].columns[c], SQL_SUG_COLUMN, word, word_len);
            }
        }
    }

    return n;
}

/* Replaces the in-progress word (word_start..+word_len) with the accepted
   suggestion plus a trailing space, and moves the cursor past it. */
static void sql_accept_suggestion(AppState *st, int word_start, int word_len, const char *suggestion) {
    struct nk_text_edit *edit = &st->sql_edit;
    if (word_len > 0) nk_str_delete_chars(&edit->string, word_start, word_len);
    int slen = (int)strlen(suggestion);
    nk_str_insert_text_char(&edit->string, word_start, suggestion, slen);
    nk_str_insert_text_char(&edit->string, word_start + slen, " ", 1);
    edit->cursor = word_start + slen + 1;
    edit->select_start = edit->select_end = edit->cursor;
}

/* Strips surrounding whitespace and one trailing ';' from a user-typed
   statement, so it can be dropped into "SELECT * FROM (<this>) AS t
   LIMIT.. OFFSET.." for pagination without a stray ';' breaking the
   subquery. */
static void sql_trim_statement(const char *in, char *out, size_t out_size) {
    int len = (int)strlen(in);
    int start = 0, end = len;
    while (start < end && isspace((unsigned char)in[start])) start++;
    while (end > start && isspace((unsigned char)in[end - 1])) end--;
    if (end > start && in[end - 1] == ';') end--;
    while (end > start && isspace((unsigned char)in[end - 1])) end--;
    int n = end - start;
    if (n >= (int)out_size) n = (int)out_size - 1;
    if (n < 0) n = 0;
    memcpy(out, in + start, (size_t)n);
    out[n] = '\0';
}

/* Only SELECT/WITH statements are safe to wrap as "SELECT * FROM (<this>)
   AS t LIMIT .. OFFSET .." for pagination - anything else (INSERT, DDL,
   PRAGMA, ...) either isn't a result set at all or can't legally appear
   inside a derived table. */
static int sql_looks_pageable(const char *trimmed) {
    return strncasecmp(trimmed, "select", 6) == 0 || strncasecmp(trimmed, "with", 4) == 0;
}

/* Runs st->sql_base_query at st->sql_offset and fills st->sql_result,
   wrapping it for pagination first when it looks like a plain SELECT/WITH
   (sql_looks_pageable). If the wrapped form fails - a construct the engine
   won't allow inside a derived table, or several statements typed at once -
   falls back to running the statement exactly as typed, unpaginated,
   rather than surfacing an error about a query the user never wrote. */
static void sql_run_current_page(AppState *st) {
    if (!st->connected || !st->sql_result) return;
    char err[MAX_ERROR_LEN];
    char query[SQL_TEXT_CAP + 128];

    if (st->sql_pageable) {
        snprintf(query, sizeof(query), "SELECT * FROM (%s) AS _dbfiller_page LIMIT %d OFFSET %d",
                 st->sql_base_query, MAX_PREVIEW_ROWS, st->sql_offset);
    } else {
        snprintf(query, sizeof(query), "%s", st->sql_base_query);
    }

    memset(st->sql_result, 0, sizeof(*st->sql_result));
    int rc = st->backend->exec_sql(st->conn, query, &st->sql_is_query, st->sql_result,
                                    &st->sql_affected, err, sizeof(err));
    if (rc != 0 && st->sql_pageable) {
        st->sql_pageable = 0;
        st->sql_offset = 0;
        memset(st->sql_result, 0, sizeof(*st->sql_result));
        rc = st->backend->exec_sql(st->conn, st->sql_base_query, &st->sql_is_query, st->sql_result,
                                    &st->sql_affected, err, sizeof(err));
    }

    if (rc != 0) {
        snprintf(st->sql_status_msg, sizeof(st->sql_status_msg), "Error: %s", err);
        st->sql_total_rows = -1;
        return;
    }

    st->sql_total_rows = -1;
    if (st->sql_is_query) {
        if (st->sql_pageable) {
            char count_query[SQL_TEXT_CAP + 64];
            snprintf(count_query, sizeof(count_query), "SELECT COUNT(*) FROM (%s) AS _dbfiller_count",
                     st->sql_base_query);
            RowPreview count_result;
            int count_is_query = 0, count_affected = 0;
            char cerr[MAX_ERROR_LEN];
            if (st->backend->exec_sql(st->conn, count_query, &count_is_query, &count_result,
                                       &count_affected, cerr, sizeof(cerr)) == 0
                && count_is_query && count_result.row_count > 0) {
                st->sql_total_rows = atoll(count_result.cells[0][0]);
            }
        }
        snprintf(st->sql_status_msg, sizeof(st->sql_status_msg), "%d fila%s.",
                 st->sql_result->row_count, st->sql_result->row_count == 1 ? "" : "s");
    } else {
        snprintf(st->sql_status_msg, sizeof(st->sql_status_msg), "OK: %d fila%s afectada%s.",
                 st->sql_affected, st->sql_affected == 1 ? "" : "s", st->sql_affected == 1 ? "" : "s");
    }
}

/* Runs whatever is currently typed into the SQL panel as one statement. A
   result set (SELECT and friends) fills st->sql_result for draw_row_grid;
   anything else just reports the affected-row count. Errors surface in
   sql_status_msg rather than aborting - a bad statement shouldn't lose
   whatever the user already typed. */
static void do_execute_sql(AppState *st) {
    if (!st->connected || !st->sql_result) return;
    char sql[SQL_TEXT_CAP];
    sql_text_snapshot(st, sql, sizeof(sql));

    char trimmed[SQL_TEXT_CAP];
    sql_trim_statement(sql, trimmed, sizeof(trimmed));
    if (trimmed[0] == '\0') {
        snprintf(st->sql_status_msg, sizeof(st->sql_status_msg), "Escribe una sentencia SQL primero.");
        return;
    }

    snprintf(st->sql_base_query, sizeof(st->sql_base_query), "%s", trimmed);
    st->sql_offset = 0;
    st->sql_pageable = sql_looks_pageable(trimmed);
    sql_run_current_page(st);

    /* A DDL/DML statement (CREATE/DROP/ALTER/INSERT/DELETE/...) can change
       what tables exist or what's in them - refresh so the sidebar, any
       open table view, and the autocomplete schema cache don't go stale. */
    if (!st->sql_is_query) {
        refresh_current(st, 1);
        sql_refresh_schema_cache(st);
    }
}

/* Draws the databases/tables tree that used to be the whole sidebar (see
   historial.md) - now nested under whichever connection row in draw_sidebar
   is currently active, at `depth_base` (1 for a top-level connection row).
   Bit-for-bit the old depth-0 behavior when depth_base is 0. */
static void draw_databases_tables_tree(struct nk_context *ctx, AppState *st, int depth_base) {
    if (st->engine == DB_ENGINE_SQLITE) {
        const char *base = strrchr(st->sqlite_path, '/');
        base = (base && base[1]) ? base + 1 : st->sqlite_path;

        nk_layout_row_template_begin(ctx, 20.0f);
        if (depth_base > 0) nk_layout_row_template_push_static(ctx, 14.0f * (float)depth_base);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        if (depth_base > 0) nk_spacing(ctx, 1);

        struct nk_rect folder_bounds;
        nk_widget(&folder_bounds, ctx); /* claims the row slot; nk_widget_bounds() alone would not */
        draw_icon_left(ctx, folder_bounds, 0.0f, ICON_FOLDER, st->icon_font,
                       muted_text_color(st->dark_theme));
        const char *root_label = base[0] ? base : "Tablas";
        struct nk_rect label_bounds = folder_bounds;
        label_bounds.x += st->icon_font ? 20.0f : 0.0f;
        label_bounds.w -= st->icon_font ? 20.0f : 0.0f;
        label_bounds.y += (folder_bounds.h - ctx->style.font->height) / 2.0f;
        nk_draw_text(nk_window_get_canvas(ctx), label_bounds, root_label, (int)strlen(root_label),
                     ctx->style.font, nk_rgba(0, 0, 0, 0), muted_text_color(st->dark_theme));

        for (int i = 0; i < st->table_count; i++) {
            int is_sel = strcmp(st->selected_table, st->tables[i]) == 0;
            if (tree_row(ctx, st->tables[i], depth_base + 1, is_sel)) select_table(st, st->tables[i]);
        }
    } else {
        for (int i = 0; i < st->database_count; i++) {
            int is_active = strcmp(st->active_database, st->databases[i]) == 0 && st->connected;
            char label[MAX_NAME_LEN + 4];
            snprintf(label, sizeof(label), "%s %s", is_active ? "v" : ">", st->databases[i]);
            if (tree_row(ctx, label, depth_base, is_active)) {
                if (is_active) close_database(st);
                else activate_database(st, st->databases[i]);
            }
            if (is_active) {
                for (int t = 0; t < st->table_count; t++) {
                    int is_sel = strcmp(st->selected_table, st->tables[t]) == 0;
                    if (tree_row(ctx, st->tables[t], depth_base + 1, is_sel)) select_table(st, st->tables[t]);
                }
            }
        }
    }
}

#ifndef _WIN32
/* Re-lists containers into st->docker_containers, or records the failure
   reason in st->docker_status_msg (docker not installed, daemon down, ...) -
   called on entering the Contenedores view, on manual "Actualizar", and
   after every start/stop/restart so the list reflects the new state. */
static void docker_refresh(AppState *st) {
    char err[256];
    int n = docker_list_containers(st->docker_containers, MAX_CONTAINERS, err, sizeof(err));
    if (n < 0) {
        snprintf(st->docker_status_msg, sizeof(st->docker_status_msg), "%s", err);
    } else {
        st->docker_container_count = n;
        st->docker_status_msg[0] = '\0';
    }
}

static void draw_containers_panel(struct nk_context *ctx, AppState *st) {
    nk_layout_row_dynamic(ctx, 28, 1);
    push_accent_button(ctx);
    if (icon_button(ctx, st->icon_font, ICON_REFRESH, "   Actualizar", nk_rgb(255, 255, 255)))
        docker_refresh(st);
    pop_button_style(ctx);

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_spacing(ctx, 1);

    for (int i = 0; i < st->docker_container_count; i++) {
        DockerContainer *c = &st->docker_containers[i];

        nk_layout_row_dynamic(ctx, 18, 1);
        char hdr[300];
        snprintf(hdr, sizeof(hdr), "%s  (%s)", c->name, c->image);
        nk_label(ctx, hdr, NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 16, 1);
        char sub[300];
        snprintf(sub, sizeof(sub), "%s%s%s", c->status, c->ports[0] ? " - " : "", c->ports);
        nk_label_colored(ctx, sub, NK_TEXT_LEFT, muted_text_color(st->dark_theme));

        if (st->docker_confirm_stop_index == i) {
            nk_layout_row_dynamic(ctx, 24, 2);
            push_danger_button(ctx);
            if (icon_button(ctx, st->icon_font, ICON_WARNING, "  Si, detener", nk_rgb(255, 255, 255))) {
                char err[256];
                if (docker_stop_container(c->id, err, sizeof(err)) != 0)
                    snprintf(st->docker_status_msg, sizeof(st->docker_status_msg), "%s", err);
                st->docker_confirm_stop_index = -1;
                docker_refresh(st);
            }
            pop_button_style(ctx);
            if (icon_button(ctx, st->icon_font, ICON_CLOSE, "  Cancelar", normal_icon_color(st->dark_theme)))
                st->docker_confirm_stop_index = -1;
        } else {
            nk_layout_row_dynamic(ctx, 24, 2);
            if (c->running) {
                push_danger_button(ctx);
                if (nk_button_label(ctx, "Detener")) st->docker_confirm_stop_index = i;
                pop_button_style(ctx);
            } else {
                push_accent_button(ctx);
                if (icon_button(ctx, st->icon_font, ICON_PLAY, "  Iniciar", nk_rgb(255, 255, 255))) {
                    char err[256];
                    if (docker_start_container(c->id, err, sizeof(err)) != 0)
                        snprintf(st->docker_status_msg, sizeof(st->docker_status_msg), "%s", err);
                    docker_refresh(st);
                }
                pop_button_style(ctx);
            }
            if (nk_button_label(ctx, "Reiniciar")) {
                char err[256];
                if (docker_restart_container(c->id, err, sizeof(err)) != 0)
                    snprintf(st->docker_status_msg, sizeof(st->docker_status_msg), "%s", err);
                docker_refresh(st);
            }
        }

        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);
    }

    if (st->docker_container_count == 0 && st->docker_status_msg[0] == '\0') {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, "No hay contenedores (o Docker no esta corriendo).", NK_TEXT_LEFT,
                          muted_text_color(st->dark_theme));
    }
}
#endif

/* Always-visible sidebar: saved connections as the top tree level (each
   expandable into its databases/tables via draw_databases_tables_tree),
   plus pinned "+ Nueva conexion" and "Contenedores" rows. Collapses to a
   thin strip via the existing chevron toggle, same as before. */
static void draw_sidebar(struct nk_context *ctx, AppState *st) {
    nk_layout_row_dynamic(ctx, 22, 1);
    if (icon_only_button(ctx, st->icon_font,
                          st->sidebar_collapsed ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_LEFT,
                          normal_icon_color(st->dark_theme)))
        st->sidebar_collapsed = !st->sidebar_collapsed;

    if (st->sidebar_collapsed) return;

    if (tree_row(ctx, "+ Nueva conexion", 0, st->view_mode == VIEW_NEW_CONNECTION))
        st->view_mode = VIEW_NEW_CONNECTION;

    for (int i = 0; i < st->saved_connection_count; i++) {
        int is_active = (i == st->active_saved_connection_index);
        char label[MAX_NAME_LEN + 4];
        snprintf(label, sizeof(label), "%s %s", is_active ? "v" : ">", st->saved_connections[i].name);
        int clicked_row = 0, clicked_delete = 0;
        tree_row_with_delete(ctx, st, label, 0, is_active, &clicked_row, &clicked_delete);
        if (clicked_delete) {
            delete_saved_connection(st, i);
            break; /* list just shifted under us - finish the redraw next frame */
        }
        if (clicked_row) {
            if (is_active) st->view_mode = VIEW_CONNECTION; /* already connected, just switch view */
            else select_saved_connection(st, i);
        }
        if (is_active) draw_databases_tables_tree(ctx, st, 1);
    }

    /* A live connection made from "Nueva conexion" that was never saved -
       still needs a row so its databases/tables have somewhere to nest,
       instead of vanishing from the sidebar entirely. */
    if (st->active_saved_connection_index == -1 && (st->connected || st->picking_db)) {
        if (tree_row(ctx, "* Conexion actual", 0, st->view_mode == VIEW_CONNECTION))
            st->view_mode = VIEW_CONNECTION;
        draw_databases_tables_tree(ctx, st, 1);
    }

#ifndef _WIN32
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    if (tree_row(ctx, "Contenedores", 0, st->view_mode == VIEW_CONTAINERS)) {
        st->view_mode = VIEW_CONTAINERS;
        docker_refresh(st);
    }
#endif
}

static void draw_ui(struct nk_context *ctx, AppState *st, int width, int height) {
    int tab_pressed = nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB);
    if (tab_pressed && st->field_count > 0) {
        st->pending_focus = (st->focused_field < 0) ? 0 : (st->focused_field + 1) % st->field_count;
    }
    int field_counter = 0;

    /* No NK_WINDOW_TITLE: the native OS window already has "dbfiller" in its
       title bar, so Nuklear's own title bar was just a redundant colored
       strip. NO_SCROLLBAR: any content that doesn't fit must be contained
       (and scrolled, if needed) by one of the inner groups - sidebar,
       mainpanel, schema_grid/data_grid, the SQL results grid - never by the
       app window itself. */
    if (!nk_begin(ctx, "dbfiller", nk_rect(0, 0, (float)width, (float)height),
                  NK_WINDOW_NO_SCROLLBAR)) {
        nk_end(ctx);
        return;
    }

    /* Sidebar + main panel are always visible now (saved connections are a
       sidebar-level concept, browsable before ever connecting to anything -
       see draw_sidebar()). What used to be two entirely different window
       layouts (a centered login form vs. header+sidebar+mainpanel) is now
       just this same split with the main panel's content switched on
       st->view_mode. */
    int panel_height = height - 110;
    if (panel_height < 220) panel_height = 220;
    float sidebar_w = st->sidebar_collapsed ? 44.0f : 220.0f;
    float main_w = (float)width - sidebar_w - 24.0f;
    if (main_w < 200.0f) main_w = 200.0f;

    /* Top row: the connected header (connection_summary + Refrescar +
       Cerrar conexion) in VIEW_CONNECTION, or a plain view title otherwise -
       same 24px height either way, so panel_height above never has to
       change per view. */
    if (st->view_mode == VIEW_CONNECTION) {
        float ratios_header[] = {0.56f, 0.22f, 0.22f};
        nk_layout_row(ctx, NK_DYNAMIC, 24, 3, ratios_header);
        nk_label(ctx, st->connection_summary, NK_TEXT_LEFT);
        if (icon_button(ctx, st->icon_font, ICON_REFRESH, "   Refrescar", normal_icon_color(st->dark_theme))) {
            refresh_current(st, 0);
            st->last_refresh_time = glfwGetTime();
        }
        if (icon_button(ctx, st->icon_font, ICON_CLOSE, "   Cerrar conexion", normal_icon_color(st->dark_theme))) {
            /* Used to nk_end()+return here to skip the rest of the window -
               not safe anymore now that the sidebar is unconditional (that
               would skip drawing it this frame). Just fall through: the
               view_mode switch below re-evaluates for the same frame, same
               as the Motor combo already does when st->engine changes. */
            close_connection(st);
            st->view_mode = VIEW_NEW_CONNECTION;
            st->active_saved_connection_index = -1;
        }
    } else {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, st->view_mode == VIEW_CONTAINERS ? "Contenedores Docker"
                                                        : "Generador Inteligente de Inserciones",
                 NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, st->view_mode == VIEW_CONTAINERS ? st->docker_status_msg : st->status_msg,
                      NK_TEXT_LEFT, status_text_color(st->dark_theme));

    nk_layout_row_begin(ctx, NK_STATIC, (float)panel_height, 2);

    /* --- Sidebar: saved connections (each expandable into its databases/
       tables), drawn as a VS Code Explorer-style tree (indentation +
       hover/selection wash) rather than a stack of buttons, so the
       parent/child hierarchy actually reads at a glance. Collapsible down
       to a thin strip when not needed, to give the main panel (and the SQL
       editor especially) more room. The default 12px group padding eats
       almost the whole collapsed width (44px), leaving no room to click the
       toggle back open - shrink it just for this narrow state. --- */
    /* Captured once, before the toggle button can flip
       st->sidebar_collapsed mid-frame: the push below and the pop after
       nk_group_end must agree on whether a push actually happened, or
       the click that closes/opens the sidebar leaves nuklear's style
       stack unbalanced (pushed but never popped) - corrupting it for
       every widget drawn afterward, including next frame. */
    int sidebar_was_collapsed = st->sidebar_collapsed;

    nk_layout_row_push(ctx, sidebar_w);
    push_surface_bg(ctx, st->dark_theme);
    if (sidebar_was_collapsed) nk_style_push_vec2(ctx, &ctx->style.window.group_padding, nk_vec2(4.0f, 8.0f));
    if (nk_group_begin(ctx, "sidebar", NK_WINDOW_BORDER)) {
        draw_sidebar(ctx, st);
        nk_group_end(ctx);
    }
    if (sidebar_was_collapsed) nk_style_pop_vec2(ctx);
    pop_surface_bg(ctx);

    /* --- Main panel: connection form, database/table detail, or the
       Docker container list, depending on st->view_mode. --- */
    nk_layout_row_push(ctx, main_w);
    push_surface_bg(ctx, st->dark_theme);
    if (nk_group_begin(ctx, "mainpanel", NK_WINDOW_BORDER)) {
        switch (st->view_mode) {
        case VIEW_NEW_CONNECTION: {
        float ratios_engine[] = {0.22f, 0.78f};
        nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_engine);
        nk_label(ctx, "Motor:", NK_TEXT_LEFT);
        {
            int sel = (int)st->engine;
            sel = nk_combo(ctx, ENGINE_LABELS, DB_ENGINE_COUNT, sel, 25, nk_vec2(200, 200));
            if (sel != (int)st->engine) {
                st->engine = (DbEngine)sel;
                snprintf(st->port_str, sizeof(st->port_str), "%d", ENGINE_DEFAULT_PORTS[sel]);
            }
        }
        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);

        if (st->engine == DB_ENGINE_SQLITE) {
            float ratios[] = {0.16f, 0.62f, 0.22f};
            nk_layout_row(ctx, NK_DYNAMIC, 25, 3, ratios);
            nk_label(ctx, "Archivo:", NK_TEXT_LEFT);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->sqlite_path, sizeof(st->sqlite_path),
                       nk_filter_default, "ruta a un archivo .db / .sqlite", 0);
            if (nk_button_label(ctx, "Examinar...")) pick_sqlite_file(st);
        } else {
            float ratios_field[] = {0.30f, 0.70f};
            nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_field);
            nk_label(ctx, "Host:", NK_TEXT_LEFT);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->host, sizeof(st->host),
                       nk_filter_default, "127.0.0.1", 0);

            nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_field);
            nk_label(ctx, "Puerto:", NK_TEXT_LEFT);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->port_str, sizeof(st->port_str),
                       nk_filter_decimal, "puerto", 0);

            nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_field);
            nk_label(ctx, "Usuario:", NK_TEXT_LEFT);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->user, sizeof(st->user),
                       nk_filter_default, "usuario", 0);

            nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_field);
            nk_label(ctx, "Contrasena:", NK_TEXT_LEFT);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, st->password, sizeof(st->password),
                       nk_filter_default, "contrasena", 1);
        }

        if (st->show_browser) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, st->browse_dir, NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 160, 1);
            push_surface_bg(ctx, st->dark_theme);
            if (nk_group_begin(ctx, "browser", NK_WINDOW_BORDER)) {
                nk_layout_row_dynamic(ctx, 20, 1);
                if (nk_button_label(ctx, ".. (subir)")) {
                    char *slash = strrchr(st->browse_dir, '/');
                    if (slash && slash != st->browse_dir) *slash = '\0';
                    else snprintf(st->browse_dir, sizeof(st->browse_dir), "/");
                    refresh_browser(st);
                }
                for (int i = 0; i < st->browse_dir_count; i++) {
                    char label[MAX_NAME_LEN + 4];
                    snprintf(label, sizeof(label), "[%s]", st->browse_dirs[i]);
                    if (nk_button_label(ctx, label)) {
                        char newdir[512];
                        snprintf(newdir, sizeof(newdir), "%s/%s", st->browse_dir, st->browse_dirs[i]);
                        snprintf(st->browse_dir, sizeof(st->browse_dir), "%s", newdir);
                        refresh_browser(st);
                    }
                }
                for (int i = 0; i < st->browse_file_count; i++) {
                    if (nk_button_label(ctx, st->browse_files[i])) {
                        snprintf(st->sqlite_path, sizeof(st->sqlite_path), "%s/%s", st->browse_dir, st->browse_files[i]);
                        st->show_browser = 0;
                    }
                }
                nk_group_end(ctx);
            }
            pop_surface_bg(ctx);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, "Cancelar")) st->show_browser = 0;
        }

        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);
        nk_layout_row_dynamic(ctx, 32, 1);
        int enter_pressed = !st->show_browser && nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER);
        push_accent_button(ctx);
        if (nk_button_label(ctx, "Conectar") || enter_pressed) do_connect(st);
        pop_button_style(ctx);

        /* "Guardar conexion": two-step, same shape as confirm_wipe - a name
           field only appears once the user has actually asked to save,
           rather than always showing an extra field in the form. */
        nk_layout_row_dynamic(ctx, 6, 1);
        nk_spacing(ctx, 1);
        if (!st->save_conn_pending) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_button_label(ctx, "Guardar conexion")) {
                st->save_conn_pending = 1;
                st->save_conn_name[0] = '\0';
            }
        } else {
            float ratios_save[] = {0.6f, 0.2f, 0.2f};
            nk_layout_row(ctx, NK_DYNAMIC, 26, 3, ratios_save);
            edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->save_conn_name, sizeof(st->save_conn_name),
                       nk_filter_default, "nombre de la conexion", 0);
            push_accent_button(ctx);
            if (nk_button_label(ctx, "Confirmar") && st->save_conn_name[0]) {
                save_current_connection(st, st->save_conn_name);
                st->save_conn_pending = 0;
            }
            pop_button_style(ctx);
            if (nk_button_label(ctx, "Cancelar")) st->save_conn_pending = 0;
        }
        break;
        }
        case VIEW_CONTAINERS: {
#ifndef _WIN32
        draw_containers_panel(ctx, st);
#endif
        break;
        }
        case VIEW_CONNECTION: {
            if (st->selected_table[0] == '\0') {
                nk_layout_row_dynamic(ctx, 20, 1);
                if (st->active_database[0] || st->engine == DB_ENGINE_SQLITE) {
                    char hdr[300];
                    snprintf(hdr, sizeof(hdr), "Base: %s",
                             st->engine == DB_ENGINE_SQLITE ? st->sqlite_path : st->active_database);
                    nk_label(ctx, hdr, NK_TEXT_LEFT);

                    float ratios_count[] = {0.40f, 0.60f};
                    nk_layout_row(ctx, NK_DYNAMIC, 25, 2, ratios_count);
                    nk_label(ctx, "Registros por tabla:", NK_TEXT_LEFT);
                    edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->count_str, sizeof(st->count_str),
                               nk_filter_decimal, "cantidad", 0);

                    nk_layout_row_dynamic(ctx, 8, 1);
                    nk_spacing(ctx, 1);
                    nk_layout_row_dynamic(ctx, 32, 1);
                    push_accent_button(ctx);
                    if (nk_button_label(ctx, "Llenar toda la base de datos")) do_fill_all(st);
                    pop_button_style(ctx);

                    nk_layout_row_dynamic(ctx, 6, 1);
                    nk_spacing(ctx, 1);
                    if (!st->confirm_wipe) {
                        nk_layout_row_dynamic(ctx, 28, 1);
                        push_danger_button(ctx);
                        if (icon_button(ctx, st->icon_font, ICON_DELETE, "   Vaciar base de datos",
                                         nk_rgb(255, 255, 255)))
                            st->confirm_wipe = 1;
                        pop_button_style(ctx);
                    } else {
                        nk_layout_row_dynamic(ctx, 18, 1);
                        nk_label_colored(ctx,
                            "Esto borra TODOS los registros de TODAS las tablas. No se puede deshacer.",
                            NK_TEXT_LEFT, status_text_color(st->dark_theme));
                        nk_layout_row_dynamic(ctx, 28, 2);
                        push_danger_button(ctx);
                        if (icon_button(ctx, st->icon_font, ICON_WARNING, "   Si, vaciar todo",
                                         nk_rgb(255, 255, 255))) {
                            do_wipe_database(st);
                            st->confirm_wipe = 0;
                        }
                        pop_button_style(ctx);
                        if (icon_button(ctx, st->icon_font, ICON_CLOSE, "   Cancelar",
                                         normal_icon_color(st->dark_theme)))
                            st->confirm_wipe = 0;
                    }

                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, "Resultado:", NK_TEXT_LEFT);
                    float log_h = (float)panel_height - 262.0f;
                    if (log_h < 80.0f) log_h = 80.0f;
                    nk_layout_row_dynamic(ctx, log_h, 1);
                    nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX | NK_EDIT_READ_ONLY, st->result_log,
                                                    sizeof(st->result_log), nk_filter_default);
                } else {
                    nk_label(ctx, "Elige una base de datos de la izquierda para comenzar.", NK_TEXT_LEFT);
                }
            } else {
                nk_layout_row_dynamic(ctx, 20, 1);
                char hdr[300];
                snprintf(hdr, sizeof(hdr), "Tabla: %s", st->selected_table);
                nk_label(ctx, hdr, NK_TEXT_LEFT);

                /* Segmented control: the active tab gets the solid accent
                   fill, the inactive ones stay in the plain flat button
                   style - reads as one control with three states, not three
                   unrelated buttons. SQL lives here (not in the sidebar)
                   because it's "one more view of this database", same as
                   Estructura/Registros. */
                nk_layout_row_dynamic(ctx, 26, 3);
                int estructura_active = st->table_view_mode == 0;
                if (estructura_active) push_accent_button(ctx);
                if (nk_button_label(ctx, "Estructura")) st->table_view_mode = 0;
                if (estructura_active) pop_button_style(ctx);

                int registros_active = st->table_view_mode == 1;
                if (registros_active) push_accent_button(ctx);
                if (nk_button_label(ctx, "Registros")) {
                    st->table_view_mode = 1;
                    load_row_preview(st);
                }
                if (registros_active) pop_button_style(ctx);

                int sql_active = st->table_view_mode == 2;
                if (sql_active) push_accent_button(ctx);
                if (icon_button(ctx, st->icon_font, ICON_SEARCH, "  SQL",
                                 sql_active ? nk_rgb(255, 255, 255) : normal_icon_color(st->dark_theme))) {
                    st->table_view_mode = 2;
                    sql_refresh_schema_cache(st);
                }
                if (sql_active) pop_button_style(ctx);

                float grid_h = (float)panel_height - 230.0f;
                if (grid_h < 100.0f) grid_h = 100.0f;

                if (st->table_view_mode == 0) {
                    nk_layout_row_dynamic(ctx, grid_h, 1);
                    if (nk_group_begin(ctx, "schema_grid", NK_WINDOW_BORDER)) {
                        nk_layout_row_dynamic(ctx, 20, 1);
                        for (int i = 0; i < st->selected_schema.column_count; i++) {
                            const DbColumn *c = &st->selected_schema.columns[i];
                            char line[256];
                            snprintf(line, sizeof(line), "%-20s %s%s%s%s%s", c->name,
                                     c->is_pk ? "PK " : "", c->is_autoincrement ? "AUTOINC " : "",
                                     c->nullable ? "" : "NOT NULL ", c->is_unique ? "UNIQUE " : "",
                                     c->has_fk ? "FK" : "");
                            nk_label(ctx, line, NK_TEXT_LEFT);
                        }
                        nk_group_end(ctx);
                    }
                } else if (st->table_view_mode == 1) {
                    nk_layout_row_dynamic(ctx, grid_h, 1);
                    if (nk_group_begin(ctx, "data_grid", NK_WINDOW_BORDER)) {
                        draw_row_grid(ctx, st->row_preview, st->dark_theme);
                        nk_group_end(ctx);
                    }
                    /* Page index: "Pagina N de M (T registros)" when the
                       total count query succeeded, otherwise just "Pagina N"
                       - count_rows can fail independently of fetch_rows
                       (e.g. a permissions quirk), so this degrades instead
                       of hiding the pager entirely. */
                    int current_page = st->preview_offset / MAX_PREVIEW_ROWS + 1;
                    long long total = st->preview_total_rows;
                    int total_pages = total >= 0 ? (int)((total + MAX_PREVIEW_ROWS - 1) / MAX_PREVIEW_ROWS) : -1;
                    if (total_pages < 1) total_pages = total >= 0 ? 1 : -1;
                    int has_prev = st->preview_offset > 0;
                    int has_next = total >= 0
                        ? (long long)st->preview_offset + MAX_PREVIEW_ROWS < total
                        : st->row_preview->row_count == MAX_PREVIEW_ROWS;

                    nk_layout_row_dynamic(ctx, 24, 3);
                    if (!has_prev) nk_widget_disable_begin(ctx);
                    if (nk_button_label(ctx, "< Anterior") && has_prev) {
                        st->preview_offset -= MAX_PREVIEW_ROWS;
                        load_row_preview(st);
                    }
                    if (!has_prev) nk_widget_disable_end(ctx);

                    char page_label[96];
                    if (total_pages >= 0)
                        snprintf(page_label, sizeof(page_label), "Pagina %d de %d (%lld registros)",
                                 current_page, total_pages, total);
                    else
                        snprintf(page_label, sizeof(page_label), "Pagina %d", current_page);
                    nk_label(ctx, page_label, NK_TEXT_CENTERED);

                    if (!has_next) nk_widget_disable_begin(ctx);
                    if (nk_button_label(ctx, "Siguiente >") && has_next) {
                        st->preview_offset += MAX_PREVIEW_ROWS;
                        load_row_preview(st);
                    }
                    if (!has_next) nk_widget_disable_end(ctx);
                } else {
                    /* Suggest-as-you-type, Tab/Up/Down/Enter driven:
                       1. Decide this frame's edit flags from LAST frame's
                          suggestion list (computed at the end of the
                          previous frame, stored in st->sql_suggest_*):
                          if there were suggestions, add NK_EDIT_SIG_ENTER
                          so Enter reports "committed" instead of inserting
                          a newline, cycle the highlighted entry ourselves on
                          Tab/Up/Down, and clear Up/Down's "pressed" state so
                          nk_edit_buffer doesn't also move the multi-line
                          cursor between lines with the list showing (Tab
                          needs no such trick - NK_EDIT_ALLOW_TAB is
                          deliberately left out, so nk_edit_buffer never
                          treats Tab as "insert 4 spaces" in the first
                          place).
                       2. Run the actual edit widget.
                       3. If Enter committed while suggestions were showing,
                          accept the highlighted one.
                       4. Recompute suggestions from the now-current cursor
                          for this frame's floating list and next frame's
                          flags; reset the highlight only when the word
                          being completed actually changed. */
                    int had_suggestions = st->sql_suggest_count > 0;
                    nk_flags sql_flags = NK_EDIT_ALWAYS_INSERT_MODE | NK_EDIT_SELECTABLE |
                                          NK_EDIT_MULTILINE | NK_EDIT_CLIPBOARD;
                    if (had_suggestions) {
                        sql_flags |= NK_EDIT_SIG_ENTER;
                        if (st->sql_suggest_count > 1) {
                            int down = nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB) ||
                                       nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN);
                            int up = nk_input_is_key_pressed(&ctx->input, NK_KEY_UP);
                            if (down)
                                st->sql_suggest_index = (st->sql_suggest_index + 1) % st->sql_suggest_count;
                            else if (up)
                                st->sql_suggest_index = (st->sql_suggest_index - 1 + st->sql_suggest_count)
                                                         % st->sql_suggest_count;
                        }
                        ctx->input.keyboard.keys[NK_KEY_UP].clicked = 0;
                        ctx->input.keyboard.keys[NK_KEY_DOWN].clicked = 0;
                    }

                    float editor_h = 140.0f;
                    nk_layout_row_dynamic(ctx, editor_h, 1);
                    struct nk_rect editor_bounds = nk_widget_bounds(ctx);
                    nk_flags ret = nk_edit_buffer(ctx, sql_flags, &st->sql_edit, nk_filter_default);

                    if ((ret & NK_EDIT_COMMITTED) && had_suggestions && st->sql_suggest_index < st->sql_suggest_count) {
                        sql_accept_suggestion(st, st->sql_word_start, st->sql_word_len,
                                               st->sql_suggestions[st->sql_suggest_index].text);
                    }

                    const char *sql_buf = nk_str_get_const(&st->sql_edit.string);
                    int sql_len = nk_str_len_char(&st->sql_edit.string);
                    int cursor = st->sql_edit.cursor;
                    if (cursor > sql_len) cursor = sql_len;
                    int new_word_start = 0;
                    int new_word_len = st->sql_edit.active ? sql_current_word(sql_buf, cursor, &new_word_start) : 0;
                    SqlSuggestion new_suggestions[SQL_SUGGEST_MAX];
                    int new_n = st->sql_edit.active
                                ? sql_collect_suggestions(st, sql_buf, new_word_start, new_word_len, new_suggestions)
                                : 0;
                    if (new_word_start != st->sql_word_start || new_word_len != st->sql_word_len)
                        st->sql_suggest_index = 0;
                    st->sql_word_start = new_word_start;
                    st->sql_word_len = new_word_len;
                    st->sql_suggest_count = new_n;
                    for (int i = 0; i < new_n; i++) st->sql_suggestions[i] = new_suggestions[i];
                    if (st->sql_suggest_index >= st->sql_suggest_count) st->sql_suggest_index = 0;

                    nk_layout_row_dynamic(ctx, 8, 1);
                    nk_spacing(ctx, 1);
                    nk_layout_row_dynamic(ctx, 28, 1);
                    push_accent_button(ctx);
                    if (icon_button(ctx, st->icon_font, ICON_PLAY, "  Ejecutar", nk_rgb(255, 255, 255)))
                        do_execute_sql(st);
                    pop_button_style(ctx);

                    if (st->sql_status_msg[0]) {
                        nk_layout_row_dynamic(ctx, 18, 1);
                        nk_label_colored(ctx, st->sql_status_msg, NK_TEXT_LEFT, status_text_color(st->dark_theme));
                    }

                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, "Resultado:", NK_TEXT_LEFT);
                    int show_sql_pager = st->sql_pageable && st->sql_is_query;
                    float sql_grid_h = (float)panel_height - editor_h - (show_sql_pager ? 290.0f : 260.0f);
                    if (sql_grid_h < 80.0f) sql_grid_h = 80.0f;
                    nk_layout_row_dynamic(ctx, sql_grid_h, 1);
                    if (nk_group_begin(ctx, "sql_result_grid", NK_WINDOW_BORDER)) {
                        draw_row_grid(ctx, st->sql_result, st->dark_theme);
                        nk_group_end(ctx);
                    }

                    /* Pagination for SELECT-shaped results, same "Pagina N de
                       M (T filas)" pattern as the Registros tab, reusing
                       sql_run_current_page to re-fetch at the new offset -
                       see sql_base_query/sql_pageable/sql_total_rows. Only
                       shown for statements sql_looks_pageable() accepted;
                       everything else (DDL/DML, PRAGMA, ...) has no concept
                       of a "next page". */
                    if (show_sql_pager) {
                        int current_page = st->sql_offset / MAX_PREVIEW_ROWS + 1;
                        long long total = st->sql_total_rows;
                        int total_pages = total >= 0 ? (int)((total + MAX_PREVIEW_ROWS - 1) / MAX_PREVIEW_ROWS) : -1;
                        if (total_pages < 1) total_pages = total >= 0 ? 1 : -1;
                        int has_prev = st->sql_offset > 0;
                        int has_next = total >= 0
                            ? (long long)st->sql_offset + MAX_PREVIEW_ROWS < total
                            : st->sql_result->row_count == MAX_PREVIEW_ROWS;

                        nk_layout_row_dynamic(ctx, 24, 3);
                        if (!has_prev) nk_widget_disable_begin(ctx);
                        if (nk_button_label(ctx, "< Anterior") && has_prev) {
                            st->sql_offset -= MAX_PREVIEW_ROWS;
                            sql_run_current_page(st);
                        }
                        if (!has_prev) nk_widget_disable_end(ctx);

                        char sql_page_label[96];
                        if (total_pages >= 0)
                            snprintf(sql_page_label, sizeof(sql_page_label), "Pagina %d de %d (%lld filas)",
                                     current_page, total_pages, total);
                        else
                            snprintf(sql_page_label, sizeof(sql_page_label), "Pagina %d", current_page);
                        nk_label(ctx, sql_page_label, NK_TEXT_CENTERED);

                        if (!has_next) nk_widget_disable_begin(ctx);
                        if (nk_button_label(ctx, "Siguiente >") && has_next) {
                            st->sql_offset += MAX_PREVIEW_ROWS;
                            sql_run_current_page(st);
                        }
                        if (!has_next) nk_widget_disable_end(ctx);
                    }

                    /* Floating suggestion list, drawn last so it paints on
                       top of the button/status/results-grid rows above -
                       anchored to the caret's own line (via
                       sql_cursor_line_offset) rather than the bottom of the
                       whole multi-line box, and pushed inline into the
                       layout (which would shove those rows down every time
                       you type). Each row gets a left-edge color bar for its
                       kind (keyword/table/column/function) and the box ends
                       in a small hint footer - both purely visual, but they
                       turn "an unlabeled list of strings" into something
                       that reads at a glance. */
                    if (st->sql_suggest_count > 0) {
                        struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                        const struct nk_style_edit *edit_style = &ctx->style.edit;
                        float caret_row_h = ctx->style.font->height + edit_style->row_padding;
                        struct nk_vec2 caret = sql_cursor_line_offset(ctx, sql_buf, cursor, caret_row_h);
                        float area_x = editor_bounds.x + edit_style->padding.x + edit_style->border;
                        float area_y = editor_bounds.y + edit_style->padding.y + edit_style->border;
                        float anchor_x = area_x + caret.x - st->sql_edit.scrollbar.x;
                        float anchor_y = area_y + caret.y - st->sql_edit.scrollbar.y + caret_row_h;

                        float row_h = 22.0f;
                        float footer_h = 18.0f;
                        float list_w = editor_bounds.w < 300.0f ? editor_bounds.w : 300.0f;
                        /* Keep the popup from overshooting the editor's right
                           edge when the caret sits far right on a wide box. */
                        float list_x = anchor_x;
                        if (list_x + list_w > editor_bounds.x + editor_bounds.w)
                            list_x = editor_bounds.x + editor_bounds.w - list_w;
                        if (list_x < editor_bounds.x) list_x = editor_bounds.x;

                        struct nk_rect list_rect = nk_rect(list_x, anchor_y, list_w,
                                                            row_h * (float)st->sql_suggest_count + footer_h);
                        struct nk_color bg = st->dark_theme ? nk_rgb(45, 45, 45) : nk_rgb(255, 255, 255);
                        struct nk_color border = st->dark_theme ? nk_rgb(80, 80, 80) : nk_rgb(190, 190, 190);
                        nk_fill_rect(canvas, list_rect, 4.0f, bg);
                        nk_stroke_rect(canvas, list_rect, 4.0f, 1.0f, border);

                        for (int i = 0; i < st->sql_suggest_count; i++) {
                            struct nk_rect row_rect = nk_rect(list_rect.x, list_rect.y + row_h * (float)i,
                                                               list_w, row_h);
                            int hovered = nk_input_is_mouse_hovering_rect(&ctx->input, row_rect);
                            if (i == st->sql_suggest_index || hovered) {
                                struct nk_color hl = nk_rgba(ACCENT_R, ACCENT_G, ACCENT_B,
                                                              i == st->sql_suggest_index ? 160 : 80);
                                nk_fill_rect(canvas, row_rect, 3.0f, hl);
                            }

                            struct nk_color kind_color;
                            switch (st->sql_suggestions[i].kind) {
                                case SQL_SUG_TABLE:    kind_color = nk_rgb(76, 175, 80); break;
                                case SQL_SUG_COLUMN:   kind_color = nk_rgb(255, 152, 0); break;
                                case SQL_SUG_FUNCTION: kind_color = nk_rgb(0, 150, 136); break;
                                default:               kind_color = nk_rgb(ACCENT_R, ACCENT_G, ACCENT_B); break;
                            }
                            struct nk_rect bar_rect = nk_rect(row_rect.x + 3.0f, row_rect.y + 3.0f,
                                                               3.0f, row_h - 6.0f);
                            nk_fill_rect(canvas, bar_rect, 1.0f, kind_color);

                            struct nk_rect text_rect = row_rect;
                            text_rect.x += 12.0f;
                            text_rect.y += (row_h - ctx->style.font->height) / 2.0f;
                            const char *label = st->sql_suggestions[i].text;
                            nk_draw_text(canvas, text_rect, label, (int)strlen(label),
                                         ctx->style.font, nk_rgba(0, 0, 0, 0), normal_icon_color(st->dark_theme));
                            if (hovered && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT)) {
                                sql_accept_suggestion(st, st->sql_word_start, st->sql_word_len, label);
                                st->sql_suggest_count = 0;
                            }
                        }

                        struct nk_rect footer_rect = nk_rect(list_rect.x, list_rect.y + row_h * (float)st->sql_suggest_count,
                                                              list_w, footer_h);
                        struct nk_color dim = st->dark_theme ? nk_rgb(150, 150, 150) : nk_rgb(120, 120, 120);
                        struct nk_rect footer_text_rect = footer_rect;
                        footer_text_rect.x += 10.0f;
                        footer_text_rect.y += (footer_h - ctx->style.font->height * 0.85f) / 2.0f;
                        const char *hint = "\xe2\x86\x91\xe2\x86\x93 navegar - Tab/Enter aceptar";
                        nk_draw_text(canvas, footer_text_rect, hint, (int)strlen(hint),
                                     ctx->style.font, nk_rgba(0, 0, 0, 0), dim);
                    }
                }

                if (st->table_view_mode != 2) {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, "Generar registros para esta tabla:", NK_TEXT_LEFT);
                    nk_layout_row_dynamic(ctx, 25, 2);
                    edit_field(ctx, st, &field_counter, NK_EDIT_FIELD, st->single_count_str, sizeof(st->single_count_str),
                               nk_filter_decimal, "cantidad", 0);
                    push_accent_button(ctx);
                    if (nk_button_label(ctx, "Generar")) do_generate_single_table(st);
                    pop_button_style(ctx);
                    if (st->table_result_msg[0]) {
                        nk_layout_row_dynamic(ctx, 20, 1);
                        nk_label_colored(ctx, st->table_result_msg, NK_TEXT_LEFT, status_text_color(st->dark_theme));
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        nk_group_end(ctx);
    }
    pop_surface_bg(ctx);
    nk_layout_row_end(ctx);

    st->field_count = field_counter;
    nk_end(ctx);
}

#define UI_FONT_PATH "third_party/fonts/LiberationSans-Regular.ttf"
#define UI_FONT_SIZE 16.0f
#define ICON_FONT_PATH "third_party/fonts/MaterialIcons-Regular.otf"
#define ICON_FONT_SIZE 16.0f

/* nk_font_atlas_add_from_file(..., NULL) bakes only Nuklear's default glyph
   range (Basic Latin + Latin-1 Supplement, ~0x0020-0x00FF) - none of our
   icon codepoints (0xE000+, see ICON_* above) are in that range, so without
   this the icon font loads "successfully" but every icon draws nothing.
   This range needs `static` storage: nk_font_atlas_add_from_file copies the
   nk_font_config struct by value, but only shallow-copies this pointer, and
   it has to stay valid until the atlas actually bakes (nk_glfw3_font_stash_
   end(), after load_font_file() has already returned). */
static const nk_rune ICON_FONT_RANGE[] = {0xE000, 0xF8FF, 0};

/* Bakes a vendored TTF/OTF at the given size, optionally restricted to a
   custom Unicode range (pass NULL for Nuklear's default range). Tries the
   path relative to the current working directory first (the documented way
   to run this binary: from the project root), then relative to the
   executable's own location - two directories up from
   build/<platform>/dbfiller-gui - so it also works when launched from
   elsewhere. Returns NULL (never fatal) if the font can't be found; callers
   just keep Nuklear's default font in that case (or, for the icon font,
   skip drawing icons - see draw_icon_left/draw_icon_centered's NULL guard). */
static struct nk_font *load_font_file(struct nk_font_atlas *atlas, const char *path,
                                       float size, const char *argv0, const nk_rune *range) {
    struct nk_font_config cfg = nk_font_config(size);
    if (range) cfg.range = range;
    const struct nk_font_config *cfg_ptr = range ? &cfg : NULL;

    struct nk_font *font = nk_font_atlas_add_from_file(atlas, path, size, cfg_ptr);
    if (font || !argv0) return font;

    char exe_dir[512];
    snprintf(exe_dir, sizeof(exe_dir), "%s", argv0);
    char *slash = strrchr(exe_dir, '/');
    if (!slash) return NULL;
    *slash = '\0';

    char full_path[600];
    snprintf(full_path, sizeof(full_path), "%s/../../%s", exe_dir, path);
    return nk_font_atlas_add_from_file(atlas, full_path, size, cfg_ptr);
}

int main(int argc, char **argv) {
    if (!glfwInit()) {
        fprintf(stderr, "No se pudo inicializar GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(WIN_W, WIN_H, "dbfiller", NULL, NULL);
    if (!win) {
        fprintf(stderr, "No se pudo crear la ventana\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);

    struct nk_context *ctx = nk_glfw3_init(win, NK_GLFW3_INSTALL_CALLBACKS);
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);
    const char *argv0 = argc > 0 ? argv[0] : NULL;
    struct nk_font *ui_font = load_font_file(atlas, UI_FONT_PATH, UI_FONT_SIZE, argv0, NULL);
    struct nk_font *icon_font = load_font_file(atlas, ICON_FONT_PATH, ICON_FONT_SIZE, argv0, ICON_FONT_RANGE);
    nk_glfw3_font_stash_end();
    if (ui_font) nk_style_set_font(ctx, &ui_font->handle);

    AppState st;
    memset(&st, 0, sizeof(st));
    st.icon_font = icon_font ? &icon_font->handle : NULL;
    st.engine = DB_ENGINE_SQLITE;
    st.focused_field = -1;
    st.pending_focus = -1;
    st.preview_total_rows = -1;
    st.sql_total_rows = -1;
    st.dark_theme = os_prefers_dark_theme();
    st.row_preview = calloc(1, sizeof(RowPreview));
    st.sql_result = calloc(1, sizeof(RowPreview));
    st.sql_schema_cache = calloc(MAX_TABLES, sizeof(SqlSchemaCache));
    nk_textedit_init_fixed(&st.sql_edit, st.sql_text, sizeof(st.sql_text));
    snprintf(st.single_count_str, sizeof(st.single_count_str), "%s", "20");
    snprintf(st.status_msg, sizeof(st.status_msg), "%s", "Sin conexion.");
    apply_theme(ctx, st.dark_theme);

    st.view_mode = VIEW_NEW_CONNECTION;
    st.active_saved_connection_index = -1;
    st.saved_connections = calloc(MAX_SAVED_CONNECTIONS, sizeof(SavedConnection));
    st.saved_connection_count = saved_connections_load(st.saved_connections, MAX_SAVED_CONNECTIONS);
#ifndef _WIN32
    st.docker_containers = calloc(MAX_CONTAINERS, sizeof(DockerContainer));
    st.docker_confirm_stop_index = -1;
#endif

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        nk_glfw3_new_frame();

        double now = glfwGetTime();
        if (st.connected && now - st.last_refresh_time >= AUTO_REFRESH_INTERVAL) {
            refresh_current(&st, 1);
            st.last_refresh_time = now;
        }
        if (now - st.last_theme_check_time >= THEME_CHECK_INTERVAL) {
            int wants_dark = os_prefers_dark_theme();
            if (wants_dark != st.dark_theme) {
                st.dark_theme = wants_dark;
                apply_theme(ctx, st.dark_theme);
            }
            st.last_theme_check_time = now;
        }

        int width, height;
        glfwGetWindowSize(win, &width, &height);
        draw_ui(ctx, &st, width, height);

        struct nk_color canvas_bg = st.dark_theme ? nk_rgb(18, 18, 18) : nk_rgb(245, 245, 245);
        glViewport(0, 0, width, height);
        glClearColor((float)canvas_bg.r / 255.0f, (float)canvas_bg.g / 255.0f, (float)canvas_bg.b / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_glfw3_render(NK_ANTI_ALIASING_ON);

        glfwSwapBuffers(win);
    }

    if (st.backend && st.conn) st.backend->disconnect(st.conn);
    if (st.schema_valid) db_table_free(&st.selected_schema);
    free(st.row_preview);
    free(st.sql_result);
    free(st.sql_schema_cache);
    free(st.saved_connections);
#ifndef _WIN32
    free(st.docker_containers);
#endif
    nk_glfw3_shutdown();
    glfwTerminate();
    return 0;
}
