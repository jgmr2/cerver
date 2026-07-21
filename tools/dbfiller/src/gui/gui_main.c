/*
 * gui/gui_main.c - interfaz grafica de dbfiller (GTK4)
 *
 * DESCRIPCION
 *     Front-end alternativo al CLI (../main.c). Tres pestanas:
 *       - Tablas: conectar a Postgres, configurar el proyecto (raiz del
 *         repo, .env), cargar su esquema y generar CRUD para las tablas
 *         que se elijan. Ninguna logica de generacion vive aca - todo
 *         pasa por dbfiller_generate_table() (../generate.h), la misma
 *         funcion que usa el CLI, para que ambos front-ends se
 *         comporten identico.
 *       - Docker: monitoreo de solo lectura (containers/stats/imagenes).
 *       - Carga: pruebas de estres con Apache Bench (ab) contra los
 *         endpoints ya registrados en routes/index.h del proyecto
 *         actual (ver scan_routes_file).
 *
 *     Simplificacion aceptada a proposito en las tres pestanas: todo
 *     corre en el hilo principal de GTK (sin hilo de fondo). Para el uso
 *     esperado (Postgres/Docker locales, unas pocas operaciones por vez)
 *     es rapido; el costo es que la ventana no responde mientras cada
 *     operacion esta en curso. pump_gtk_events() entre pasos largos
 *     mitiga esto repintando la UI sin un hilo aparte. Ver README.md.
 */
#include <gtk/gtk.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../introspect.h"
#include "../generate.h"
#include "../scaffold.h"
#include "../testdb.h"

/* DiscoveredRoute - una linea get()/post()/..._auth() ya encontrada en
   routes/index.h del proyecto actual (ver scan_routes_file). La pestana
   Carga arma un desplegable con estas para poder elegir "que endpoint
   pego" en vez de escribir el path a mano sin saber cuales existen. */
#define MAX_DISCOVERED_ROUTES 128
typedef struct {
    char method[8];
    char path[256];
    int requires_auth;
} DiscoveredRoute;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;

    /* Compartido por todas las pestanas (conexion + raiz del repo). */
    GtkWidget *db_url_entry;
    GtkWidget *repo_root_entry;
    GtkWidget *force_check;
    GtkWidget *status_label;
    PGconn *conn;

    /* Panel de variables de entorno (.env) del proyecto en "Raiz del
       repo", ver on_save_env_clicked/on_generate_env_defaults_clicked. */
    GtkWidget *env_user_entry;
    GtkWidget *env_password_entry;
    GtkWidget *env_db_entry;
    GtkWidget *env_jwt_entry;
    GtkWidget *env_overwrite_check;

    /* Pestana Tablas: wizard de 6 pasos (Conexion/Proyecto/.env/Esquema/
       Tablas/Probar endpoints) - ver el comentario grande al principio
       de build_tables_tab. */
    GtkWidget *wizard_stack;
    GtkWidget *wizard_step_label;
    GtkWidget *wizard_back_btn;
    GtkWidget *wizard_next_btn;
    int wizard_step;
    GtkWidget *database_listbox; /* bases del servidor tras "Listar bases", ver on_list_databases_clicked */
    GtkWidget *schema_listbox;   /* esquemas .sql cargados en db/init/, ver refresh_schema_list */
    GtkWidget *table_listbox;
    GtkWidget *wizard_test_listbox;    /* paso "Probar endpoints" */
    GtkWidget *wizard_test_auth_entry;
    GtkTextBuffer *log_buffer;
    GtkWidget *log_view;

    /* Pestana Docker */
    GtkTextBuffer *docker_containers_buffer;
    GtkWidget *docker_containers_view;
    GtkTextBuffer *docker_stats_buffer;
    GtkWidget *docker_stats_view;
    GtkTextBuffer *docker_images_buffer;
    GtkWidget *docker_images_view;
    GtkWidget *docker_live_check;
    guint docker_live_timer_id; /* 0 = "actualizar en vivo" apagado, ver on_docker_live_toggled */

    /* Pestana Carga (ab) */
    GtkWidget *load_endpoint_dropdown; /* endpoints de routes/index.h del proyecto actual, ver scan_routes_file */
    DiscoveredRoute load_endpoints[MAX_DISCOVERED_ROUTES];
    int load_endpoint_count;
    GtkWidget *load_path_entry;
    GtkWidget *load_method_dropdown;
    GtkWidget *load_body_view;
    GtkWidget *load_auth_entry;
    GtkWidget *load_requests_spin;
    GtkWidget *load_concurrency_spin;
    GtkWidget *load_keepalive_check;
    GtkTextBuffer *load_output_buffer;
    GtkWidget *load_output_view;
} AppState;

/* ======================================================================
 * Utilidades compartidas por las cuatro pestanas
 * ====================================================================== */

/* append_line_to_buffer - agrega una linea a un GtkTextBuffer cualquiera
   y hace scroll al final de su GtkTextView. Generico (no asume
   state->log_buffer) para que cada pestana pueda tener su propio panel
   de salida sin duplicar esta logica. */
static void append_line_to_buffer(GtkTextBuffer *buffer, GtkTextView *view, const char *line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, line, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_move_mark(buffer, mark, &end);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, FALSE, 0.0, 1.0);
}

/* append_log - atajo para el panel de resultados de la pestana Tablas. */
static void append_log(AppState *state, const char *line) {
    append_line_to_buffer(state->log_buffer, GTK_TEXT_VIEW(state->log_view), line);
}

static void set_status(AppState *state, const char *text) {
    gtk_label_set_text(GTK_LABEL(state->status_label), text);
}

/* pump_gtk_events - procesa los eventos pendientes del loop de GTK sin
   bloquear. Se usa entre pasos de una operacion sincronica larga para
   que la UI se repinte mientras la operacion sigue corriendo, en vez de
   quedar congelada hasta que termine todo. */
static void pump_gtk_events(void) {
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

/* run_streaming_command - corre 'cmd' via el shell y va agregando cada
   linea de su salida (el llamador arma 'cmd' con "... 2>&1" para
   combinar stdout/stderr) a buffer/view a medida que llega, sin
   bloquear el loop de GTK entre linea y linea. Devuelve el codigo de
   salida del proceso (0 = exito, ver pclose()), o -1 si no se pudo
   lanzar el proceso. */
static int run_streaming_command(const char *cmd, GtkTextBuffer *buffer, GtkTextView *view) {
    FILE *proc = popen(cmd, "r");
    if (!proc) {
        append_line_to_buffer(buffer, view, "No se pudo lanzar el comando (popen fallo).");
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), proc)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        append_line_to_buffer(buffer, view, line);
        pump_gtk_events();
    }

    return pclose(proc);
}

/* read_env_value - busca "KEY=valor" en <repo_root>/.env (formato plano
   de una asignacion por linea, sin espacios alrededor del '=', igual al
   .env real del proyecto). Copia el valor a out. Devuelve 1 si la
   encontro, 0 si no (out queda vacio). */
static int read_env_value(const char *repo_root, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char path[1024];
    snprintf(path, sizeof(path), "%s/.env", repo_root);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    size_t key_len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            char *val = line + key_len + 1;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r')) val[--len] = '\0';
            snprintf(out, out_sz, "%s", val);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static const char *current_repo_root(AppState *state) {
    const char *repo_root = gtk_editable_get_text(GTK_EDITABLE(state->repo_root_entry));
    return *repo_root ? repo_root : ".";
}

/* default_repo_root - precarga "Raiz del repo" con la raiz real de
   cerver en vez de ".", que solo funciona si el binario se lanzo
   justo desde ahi. dbfiller vive siempre en <repo>/tools/dbfiller/build/
   (convencion de este proyecto - ver "Crear proyecto nuevo"/scaffold.c,
   que arma exactamente esa estructura), asi que basta con resolver donde
   esta el propio ejecutable (/proc/self/exe) y subir tres niveles
   (build -> dbfiller -> tools -> raiz). Si algo de esto falla (no-Linux,
   /proc no montado, etc.) cae a "." - el campo sigue siendo editable a
   mano en cualquier caso, esto es solo un mejor punto de partida. */
static void default_repo_root(char *out, size_t out_size) {
    char exe_path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0) { snprintf(out, out_size, "."); return; }
    exe_path[n] = '\0';

    char *slash = strrchr(exe_path, '/');
    if (slash) *slash = '\0'; /* exe_path: .../tools/dbfiller/build */

    char candidate[PATH_MAX + 16];
    snprintf(candidate, sizeof(candidate), "%s/../../..", exe_path);

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved)) {
        snprintf(out, out_size, "%s", resolved);
    } else {
        snprintf(out, out_size, ".");
    }
}

/* row_box - fila horizontal generica de formulario: label + widget +
   (opcional) boton. */
static GtkWidget *row_box(const char *label_text, GtkWidget *entry, GtkWidget *button) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_size_request(label, 120, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    if (button) gtk_box_append(GTK_BOX(box), button);
    return box;
}

/* labeled_output - GtkTextView de solo lectura con su propio label y
   scroll, patron repetido en Docker/Carga para cada panel de salida.
   label_text puede ser NULL cuando el llamador ya puso su propia fila de
   label (p.ej. una con un boton "Copiar logs" al lado, ver la pestana
   Carga) - en ese caso no se agrega un label vacio de mas. */
static GtkWidget *labeled_output(const char *label_text, int min_height, GtkTextBuffer **out_buffer, GtkWidget **out_view) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (label_text) {
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_box_append(GTK_BOX(box), label);
    }

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_size_request(scroll, -1, min_height);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    *out_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    *out_view = view;
    return box;
}

/* ======================================================================
 * Pestana Tablas
 * ====================================================================== */

static void clear_table_list(AppState *state) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(state->table_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(state->table_listbox), child);
    }
}

static void clear_database_list(AppState *state) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(state->database_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(state->database_listbox), child);
    }
}

/* Adelantada: on_database_row_activated (mas abajo) reutiliza este mismo
   flujo despues de armar DATABASE_URL con la base elegida. */
static void on_connect_clicked(GtkButton *button, gpointer user_data);

/* Adelantadas: la pestana "Esquema" se fusiono con "Tablas" (viven en el
   mismo panel: conectar/configurar la base y despues cargar su esquema
   son parte del mismo flujo) - build_tables_tab, definida antes que
   estas, ya conecta sus botones. */
static void on_load_schema_clicked(GtkButton *button, gpointer user_data);
static void on_refresh_schema_list_clicked(GtkButton *button, gpointer user_data);

/* Adelantada: build_tables_tab (el wizard) y el ultimo paso "Probar
   endpoints" reusan el mismo escaneo de routes/index.h que la pestana
   Carga (definida mas abajo) usa para su desplegable de endpoints. */
static int scan_routes_file(const char *repo_root, DiscoveredRoute *out, int max_routes);

/* current_db_url - DATABASE_URL del formulario, o la variable de entorno
   del mismo nombre si el campo esta vacio (mismo fallback que ya hacia
   on_connect_clicked al llamar pg_connect). Centralizado aca porque
   ahora dos flujos lo necesitan: conectar directo, y listar bases antes
   de elegir una. */
static const char *current_db_url(AppState *state) {
    const char *db_url = gtk_editable_get_text(GTK_EDITABLE(state->db_url_entry));
    if (*db_url) return db_url;
    const char *env_url = getenv("DATABASE_URL");
    return env_url ? env_url : "";
}

/*
 * on_list_databases_clicked - conecta al servidor (usando el host/
 * usuario/contrasena de DATABASE_URL pero forzando dbname=postgres, ver
 * pg_conninfo_with_dbname en introspect.h) solo para consultar
 * pg_database, y muestra el resultado en database_listbox. No toca
 * state->conn ni la lista de tablas -- es un paso previo e independiente
 * a "Conectar", pensado para cuando no se sabe (o no se quiere escribir a
 * mano) el nombre exacto de la base.
 */
static void on_list_databases_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    clear_database_list(state);

    const char *raw_url = current_db_url(state);
    if (!*raw_url) {
        append_log(state, "Escribi host/usuario/contrasena en DATABASE_URL primero (el nombre de base se ignora para este paso).");
        return;
    }

    char err[MAX_ERROR_LEN];
    char admin_conninfo[700];
    if (pg_conninfo_with_dbname(raw_url, "postgres", admin_conninfo, sizeof(admin_conninfo), err, sizeof(err)) != 0) {
        char msg[MAX_ERROR_LEN + 64];
        snprintf(msg, sizeof(msg), "DATABASE_URL invalida: %s", err);
        append_log(state, msg);
        return;
    }

    PGconn *admin_conn = pg_connect(admin_conninfo, err, sizeof(err));
    if (!admin_conn) {
        char msg[MAX_ERROR_LEN + 64];
        snprintf(msg, sizeof(msg), "Error al conectar para listar bases: %s", err);
        append_log(state, msg);
        return;
    }

    char names[MAX_TABLES][MAX_NAME_LEN];
    int n = pg_list_databases(admin_conn, names, MAX_TABLES, err, sizeof(err));
    PQfinish(admin_conn);
    if (n < 0) {
        char msg[MAX_ERROR_LEN + 64];
        snprintf(msg, sizeof(msg), "Error al listar bases: %s", err);
        append_log(state, msg);
        return;
    }

    for (int i = 0; i < n; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "database-name", g_strdup(names[i]), g_free);
        gtk_list_box_append(GTK_LIST_BOX(state->database_listbox), row);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "%d base%s encontrada%s -- elegi una para conectar", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
    append_log(state, msg);
}

/*
 * on_database_row_activated - al elegir una base de database_listbox,
 * arma DATABASE_URL apuntando ahi (mismo host/usuario/contrasena, dbname
 * reemplazado - ver pg_conninfo_with_dbname), la deja cargada en el
 * campo (visible/editable, no un estado oculto) y dispara el mismo flujo
 * de "Conectar" (on_connect_clicked) para listar sus tablas de una vez.
 */
static void on_database_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    AppState *state = user_data;
    const char *db_name = g_object_get_data(G_OBJECT(row), "database-name");
    if (!db_name) return;

    const char *raw_url = current_db_url(state);
    char err[MAX_ERROR_LEN];
    char new_conninfo[700];
    if (!*raw_url || pg_conninfo_with_dbname(raw_url, db_name, new_conninfo, sizeof(new_conninfo), err, sizeof(err)) != 0) {
        append_log(state, "No se pudo armar la conexion para esa base.");
        return;
    }

    gtk_editable_set_text(GTK_EDITABLE(state->db_url_entry), new_conninfo);
    on_connect_clicked(NULL, state);
}

/* on_connect_clicked - (re)conecta con el DATABASE_URL del formulario y
   repuebla la lista de tablas. Si ya habia una conexion abierta la
   cierra primero, para no acumular conexiones si se aprieta "Conectar"
   varias veces. */
static void on_connect_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    if (state->conn) {
        PQfinish(state->conn);
        state->conn = NULL;
    }
    clear_table_list(state);

    const char *db_url = gtk_editable_get_text(GTK_EDITABLE(state->db_url_entry));
    char err[MAX_ERROR_LEN];
    PGconn *conn = pg_connect(*db_url ? db_url : NULL, err, sizeof(err));
    if (!conn) {
        char msg[MAX_ERROR_LEN + 32];
        snprintf(msg, sizeof(msg), "Error al conectar: %s", err);
        set_status(state, msg);
        append_log(state, msg);
        return;
    }

    char names[MAX_TABLES][MAX_NAME_LEN];
    int n = pg_list_tables(conn, names, MAX_TABLES, err, sizeof(err));
    if (n < 0) {
        char msg[MAX_ERROR_LEN + 32];
        snprintf(msg, sizeof(msg), "Error al listar tablas: %s", err);
        set_status(state, msg);
        append_log(state, msg);
        PQfinish(conn);
        return;
    }

    state->conn = conn;
    for (int i = 0; i < n; i++) {
        GtkWidget *check = gtk_check_button_new_with_label(names[i]);
        gtk_list_box_append(GTK_LIST_BOX(state->table_listbox), check);
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Conectado -- %d tabla%s en 'public'", n, n == 1 ? "" : "s");
    set_status(state, msg);
    append_log(state, msg);
}

/* for_each_table_check - recorre los GtkCheckButton de la lista de
   tablas (uno por fila de table_listbox) invocando fn(check, data) */
static void for_each_table_check(AppState *state, void (*fn)(GtkCheckButton *, gpointer), gpointer data) {
    for (GtkWidget *row = gtk_widget_get_first_child(state->table_listbox); row; row = gtk_widget_get_next_sibling(row)) {
        GtkWidget *check = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        fn(GTK_CHECK_BUTTON(check), data);
    }
}

static void set_check_active(GtkCheckButton *check, gpointer data) {
    gtk_check_button_set_active(check, GPOINTER_TO_INT(data));
}

static void on_select_all_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    for_each_table_check(user_data, set_check_active, GINT_TO_POINTER(TRUE));
}

static void on_select_none_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    for_each_table_check(user_data, set_check_active, GINT_TO_POINTER(FALSE));
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppState *state = user_data;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (folder) {
        char *path = g_file_get_path(folder);
        if (path) {
            gtk_editable_set_text(GTK_EDITABLE(state->repo_root_entry), path);
            g_free(path);
        }
        g_object_unref(folder);
    }
    g_object_unref(source);
}

static void on_choose_folder_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Elegir raiz del repo");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(state->window), NULL, on_folder_chosen, state);
}

/* on_scaffold_clicked - crea el proyecto nuevo (scaffold_new_project,
   ../scaffold.h) directo en "Raiz del repo". Antes abria un segundo
   selector de carpeta propio, independiente del campo "Raiz del repo" de
   arriba (con su boton "Elegir carpeta..." ya existente) - confuso,
   obligaba a elegir la misma carpeta dos veces por dos caminos distintos.
   Ahora usa un solo campo para las dos cosas: elegis (o escribis) la
   carpeta destino ahi, y "Crear proyecto nuevo" trabaja sobre esa misma
   ruta. */
static void on_scaffold_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    const char *repo_root = current_repo_root(state);

    char err[MAX_ERROR_LEN];
    char msg[MAX_ERROR_LEN + 128];
    if (scaffold_new_project(repo_root, err, sizeof(err)) != 0) {
        snprintf(msg, sizeof(msg), "Error al crear el proyecto en '%s': %s", repo_root, err);
        append_log(state, msg);
        return;
    }

    snprintf(msg, sizeof(msg), "Proyecto nuevo creado en '%s' -- completa las variables de entorno abajo "
                                "y presiona \"Guardar .env\".", repo_root);
    append_log(state, msg);
}

/* random_hex - n_bytes de /dev/urandom, hex-encodados en out (necesita
   al menos n_bytes*2+1 bytes). Para POSTGRES_PASSWORD/JWT_SECRET por
   defecto hace falta aleatoriedad real, no g_random_int (generador
   pseudoaleatorio de proposito general, no apto para secretos) - por
   eso se lee directo de /dev/urandom en vez de usar la API de GLib, sin
   depender de tener 'openssl' instalado (mismo resultado que
   `openssl rand -hex N`, que es lo que ya sugiere el comentario de
   JWT_SECRET en .env.example). Retorna 0 en exito, -1 si algo fallo. */
static int random_hex(char *out, size_t out_size, int n_bytes) {
    unsigned char buf[64];
    if ((size_t)n_bytes > sizeof(buf) || out_size < (size_t)(n_bytes * 2 + 1)) return -1;

    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, (size_t)n_bytes, f);
    fclose(f);
    if (got != (size_t)n_bytes) return -1;

    for (int i = 0; i < n_bytes; i++) snprintf(out + i * 2, 3, "%02x", buf[i]);
    return 0;
}

/* on_generate_env_defaults_clicked - completa el panel de .env con
   valores razonables: usuario/base fijos ("app", suficiente para un
   proyecto nuevo de un solo cliente) y password/JWT_SECRET aleatorios de
   verdad (ver random_hex). No escribe nada a disco todavia -- "Guardar
   .env" (on_save_env_clicked) es un paso aparte, para poder revisar o
   editar a mano antes de confirmar. */
static void on_generate_env_defaults_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    gtk_editable_set_text(GTK_EDITABLE(state->env_user_entry), "app");
    gtk_editable_set_text(GTK_EDITABLE(state->env_db_entry), "app");

    char password[32];
    if (random_hex(password, sizeof(password), 12) == 0) {
        gtk_editable_set_text(GTK_EDITABLE(state->env_password_entry), password);
    }

    char secret[80];
    if (random_hex(secret, sizeof(secret), 32) == 0) {
        gtk_editable_set_text(GTK_EDITABLE(state->env_jwt_entry), secret);
    }

    append_log(state, "Valores por defecto generados (POSTGRES_PASSWORD y JWT_SECRET son aleatorios de verdad, no editar a mano).");
}

/*
 * on_save_env_clicked - escribe <repo_root>/.env con los valores del
 * panel, mismo formato que scaffold.c genera en .env.example (incluido
 * el DATABASE_URL vía Unix Domain Socket, ver docker-compose.yml).
 *
 * Se niega a pisar un .env que ya exista salvo que "Sobreescribir .env
 * existente" este tildado -- a diferencia de los archivos generados por
 * codegen (que llevan su propio marcador y --force), .env no tiene forma
 * de distinguir "vacio de fabrica" de "credenciales reales de un
 * proyecto en uso", y no esta en git (ver .gitignore) para poder
 * recuperarlo si se pisa sin querer.
 */
static void on_save_env_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    const char *repo_root = current_repo_root(state);

    const char *user = gtk_editable_get_text(GTK_EDITABLE(state->env_user_entry));
    const char *password = gtk_editable_get_text(GTK_EDITABLE(state->env_password_entry));
    const char *db = gtk_editable_get_text(GTK_EDITABLE(state->env_db_entry));
    const char *jwt = gtk_editable_get_text(GTK_EDITABLE(state->env_jwt_entry));

    if (!*user || !*password || !*db || !*jwt) {
        append_log(state, "Falta completar POSTGRES_USER/POSTGRES_PASSWORD/POSTGRES_DB/JWT_SECRET "
                           "(o usa \"Generar valores por defecto\").");
        return;
    }

    char env_path[1200];
    snprintf(env_path, sizeof(env_path), "%s/.env", repo_root);

    gboolean overwrite = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->env_overwrite_check));
    if (g_file_test(env_path, G_FILE_TEST_EXISTS) && !overwrite) {
        char msg[1300];
        snprintf(msg, sizeof(msg), "Ya existe '%s' -- tilda \"Sobreescribir .env existente\" si de verdad "
                                    "queres reemplazarlo.", env_path);
        append_log(state, msg);
        return;
    }

    char content[4096];
    snprintf(content, sizeof(content),
        "# Credenciales\n"
        "POSTGRES_USER=%s\n"
        "POSTGRES_PASSWORD=%s\n"
        "POSTGRES_DB=%s\n"
        "\n"
        "# Unix Domain Socket compartido con el contenedor de Postgres (ver\n"
        "# docker-compose.yml, volumen pg_socket) - no lleva host:puerto.\n"
        "DATABASE_URL=postgres://%s:%s@/%s?host=/var/run/postgresql\n"
        "\n"
        "# Secreto para firmar/verificar JWT (HS256, ver utils/auth/jwt.c).\n"
        "JWT_SECRET=%s\n"
        "\n"
        "# Tunables operativos opcionales - si no se definen aca, docker-compose.yml\n"
        "# les pone el default que se ve al lado de cada una.\n"
        "# PORT=8080\n"
        "# SHUTDOWN_GRACE_SECONDS=5\n"
        "# CACHE_REFRESH_SECONDS=30\n"
        "# DB_CONNECT_TIMEOUT_SECONDS=3\n"
        "# JWT_EXPIRES_SECONDS=86400\n"
        "# PBKDF2_ITERATIONS=100000\n"
        "# DB_POOL_SIZE=16\n"
        "# DB_PENDING_QUEUE_SIZE=8192\n",
        user, password, db, user, password, db, jwt);

    GError *error = NULL;
    if (!g_file_set_contents(env_path, content, -1, &error)) {
        char msg[1400];
        snprintf(msg, sizeof(msg), "No se pudo escribir '%s': %s", env_path, error ? error->message : "?");
        append_log(state, msg);
        if (error) g_error_free(error);
        return;
    }

    char msg[1500];
    snprintf(msg, sizeof(msg), "Guardado '%s'. Reinicia el contenedor 'db' si ya estaba corriendo con otras "
                                "credenciales (docker compose up -d --force-recreate db).", env_path);
    append_log(state, msg);
}

/* on_testdb_start_clicked - levanta el Postgres desechable de pruebas
   internas (../testdb.h) y, si arranco bien, precarga su DATABASE_URL en
   el formulario. No toca ninguna conexion externa que el usuario ya haya
   escrito ahi -- es aditivo, el campo sigue siendo editable a mano como
   siempre. */
static void on_testdb_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    char compose_path[512];
    if (testdb_write_compose_file(compose_path, sizeof(compose_path)) != 0) {
        append_log(state, "No se pudo escribir el archivo de compose temporal del Postgres de prueba.");
        return;
    }

    append_log(state, "--- levantando Postgres de prueba (dbfiller) ---");
    pump_gtk_events();

    char *quoted_compose = g_shell_quote(compose_path);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "docker compose -f %s -p " TESTDB_COMPOSE_PROJECT " up -d --wait 2>&1", quoted_compose);
    g_free(quoted_compose);

    int rc = run_streaming_command(cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
    if (rc == 0) {
        gtk_editable_set_text(GTK_EDITABLE(state->db_url_entry), TESTDB_CONNINFO);
        append_log(state, "--- Postgres de prueba arriba -- DATABASE_URL completado, presiona \"Conectar\" ---");
    } else {
        append_log(state, "--- fallo al levantar el Postgres de prueba (ver arriba) ---");
    }
}

/* on_testdb_stop_clicked - baja y descarta el Postgres de prueba (-v: el
   volumen es tmpfs igual, pero se limpia el contenedor por completo). */
static void on_testdb_stop_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    char compose_path[512];
    if (testdb_write_compose_file(compose_path, sizeof(compose_path)) != 0) {
        append_log(state, "No se pudo escribir el archivo de compose temporal del Postgres de prueba.");
        return;
    }

    append_log(state, "--- deteniendo Postgres de prueba (dbfiller) ---");
    pump_gtk_events();

    char *quoted_compose = g_shell_quote(compose_path);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "docker compose -f %s -p " TESTDB_COMPOSE_PROJECT " down -v 2>&1", quoted_compose);
    g_free(quoted_compose);

    run_streaming_command(cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
    append_log(state, "--- Postgres de prueba detenido ---");
}

/* generate_one_checked - si 'check' esta marcado, genera esa tabla
   (dbfiller_generate_table, ../generate.h -- la misma funcion que usa
   el CLI) y agrega el resultado al panel de resultados. */
static void generate_one_checked(GtkCheckButton *check, gpointer user_data) {
    AppState *state = user_data;
    if (!gtk_check_button_get_active(check)) return;

    const char *table_name = gtk_check_button_get_label(check);
    const char *repo_root = current_repo_root(state);
    int force = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->force_check));

    GenerateResult res;
    dbfiller_generate_table(state->conn, table_name, repo_root, force, &res);

    char line[MAX_ERROR_LEN + 128];
    if (res.ok) {
        snprintf(line, sizeof(line), "%s: OK (controllers/%s.c/.h, models/%s.c/.h, rutas registradas%s)",
                 table_name, table_name, table_name, res.has_update ? "" : ", sin update: no tiene columnas actualizables");
    } else {
        snprintf(line, sizeof(line), "%s: %s", table_name, res.message);
    }
    append_log(state, line);
}

static void on_generate_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    if (!state->conn) {
        append_log(state, "Conecta primero (boton \"Conectar\").");
        return;
    }
    append_log(state, "--- generando tablas seleccionadas ---");
    for_each_table_check(state, generate_one_checked, state);
}

/*
 * on_build_clicked - corre "docker compose build backend" en la raiz
 * del repo. El proyecto se compila dentro de un contenedor propio (ver
 * Dockerfile: build de libpq/liburing estaticos en un stage Alpine
 * aparte) - un "make" nativo en el host no tiene por que tener esas
 * dependencias instaladas, asi que la GUI usa el mismo camino de build
 * que ya usa todo el resto del proyecto (docker-compose.yml).
 */
static void on_build_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    const char *repo_root = current_repo_root(state);

    append_log(state, "--- docker compose build backend ---");
    set_status(state, "Compilando (Docker)...");
    pump_gtk_events();

    char *quoted_root = g_shell_quote(repo_root);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "cd %s && docker compose build backend 2>&1", quoted_root);
    g_free(quoted_root);

    int rc = run_streaming_command(cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
    if (rc == 0) {
        append_log(state, "--- docker compose build: OK ---");
        set_status(state, "Compilacion exitosa");
    } else {
        append_log(state, "--- docker compose build: fallo (ver resultados arriba) ---");
        set_status(state, "Error al compilar");
    }
}

/* copy_buffer_to_clipboard - copia el contenido completo de un
   GtkTextBuffer cualquiera al portapapeles del sistema. Generico (no
   asume state->log_buffer) para que cada pestana con panel de resultados
   (Tablas, Carga) tenga su propio boton "Copiar logs" sin duplicar esta
   logica. */
static void copy_buffer_to_clipboard(GtkTextBuffer *buffer, GtkWidget *view) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GdkClipboard *clipboard = gtk_widget_get_clipboard(view);
    gdk_clipboard_set_text(clipboard, text);
    g_free(text);
}

static void on_copy_logs_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    copy_buffer_to_clipboard(state->log_buffer, state->log_view);
    append_log(state, "(logs copiados al portapapeles)");
}

/* on_copy_load_logs_clicked - mismo boton "Copiar logs" que la pestana
   Tablas, pero para el panel de resultados de la pestana Carga
   (benchmarks de Apache Bench). */
static void on_copy_load_logs_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    copy_buffer_to_clipboard(state->load_output_buffer, state->load_output_view);
    append_line_to_buffer(state->load_output_buffer, GTK_TEXT_VIEW(state->load_output_view),
                           "(logs copiados al portapapeles)");
}

/* section_frame - GtkFrame con titulo + una caja vertical adentro para su
   contenido, mismo patron repetido para cada bloque de build_tables_tab
   (Conexion/Proyecto/.env/Esquema/Tablas). Antes todos estos bloques
   vivian sueltos, uno debajo del otro, sin ningun borde ni agrupacion
   visual que separara "estoy configurando la conexion" de "estoy
   configurando el proyecto" de "estoy generando tablas" - con ~25
   controles en una sola pestana, la distribucion de botones se volvia
   dificil de leer de un vistazo. out_box queda listo para que el
   llamador le agregue directamente sus widgets. */
static GtkWidget *section_frame(const char *title, GtkWidget **out_box) {
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    *out_box = box;
    return frame;
}

/* button_row - fila horizontal de botones de ancho parejo (homogeneous):
   antes cada fila de acciones repartia sus botones con anchos distintos
   segun tuvieran o no hexpand seteado a mano, dando una distribucion
   dispareja entre filas. Con homogeneous todos los botones de la fila
   ocupan el mismo ancho, sin tener que acordarse de poner hexpand en
   cada uno. */
static GtkWidget *button_row(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
    return box;
}

/* ======================================================================
 * Wizard de "Tablas": Conexion -> Proyecto -> .env -> Esquema -> Tablas
 * -> Probar endpoints, un paso a la vez (GtkStack) en vez de las ~30
 * controles de las 6 secciones todas juntas de una. Atras/Siguiente son
 * navegacion pura -- ningun paso se valida ni se bloquea, los botones de
 * accion de cada seccion (Conectar, Guardar .env, Generar seleccionadas,
 * etc.) siguen siendo los que hacen el trabajo real.
 * ====================================================================== */

#define WIZARD_STEP_COUNT 6
static const char *const WIZARD_STEP_NAMES[WIZARD_STEP_COUNT] = {
    "Conexion", "Proyecto", "Variables de entorno", "Esquema", "Tablas", "Probar endpoints",
};

static void wizard_update_nav(AppState *state) {
    char label[80];
    snprintf(label, sizeof(label), "Paso %d de %d -- %s",
             state->wizard_step + 1, WIZARD_STEP_COUNT, WIZARD_STEP_NAMES[state->wizard_step]);
    gtk_label_set_text(GTK_LABEL(state->wizard_step_label), label);
    gtk_widget_set_sensitive(state->wizard_back_btn, state->wizard_step > 0);
    gtk_widget_set_sensitive(state->wizard_next_btn, state->wizard_step < WIZARD_STEP_COUNT - 1);
}

/* Adelantada: refresh_wizard_test_list (justo abajo) conecta cada boton
   "Probar" de una fila con esta, definida mas abajo. */
static void on_wizard_probe_clicked(GtkButton *button, gpointer user_data);

/* refresh_wizard_test_list - repuebla el paso "Probar endpoints" con lo
   que haya ahora mismo en routes/index.h del proyecto actual (mismo
   escaneo que usa la pestana Carga, ver scan_routes_file). Se llama cada
   vez que se entra a este paso, para no mostrar una lista vieja despues
   de generar tablas nuevas. */
static void refresh_wizard_test_list(AppState *state) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(state->wizard_test_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(state->wizard_test_listbox), child);
    }

    state->load_endpoint_count = scan_routes_file(current_repo_root(state), state->load_endpoints, MAX_DISCOVERED_ROUTES);

    if (state->load_endpoint_count == 0) {
        GtkWidget *empty = gtk_label_new("(sin endpoints todavia -- genera tablas en el paso anterior)");
        gtk_list_box_append(GTK_LIST_BOX(state->wizard_test_listbox), empty);
        return;
    }

    for (int i = 0; i < state->load_endpoint_count; i++) {
        const DiscoveredRoute *r = &state->load_endpoints[i];
        char label_text[300];
        snprintf(label_text, sizeof(label_text), "%-6s %s%s", r->method, r->path, r->requires_auth ? "  (JWT)" : "");

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_hexpand(label, TRUE);
        GtkWidget *probe_btn = gtk_button_new_with_label("Probar");
        g_object_set_data(G_OBJECT(probe_btn), "route-index", GINT_TO_POINTER(i));
        g_signal_connect(probe_btn, "clicked", G_CALLBACK(on_wizard_probe_clicked), state);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), probe_btn);
        gtk_list_box_append(GTK_LIST_BOX(state->wizard_test_listbox), row);
    }
}

static void goto_wizard_step(AppState *state, int step) {
    if (step < 0 || step >= WIZARD_STEP_COUNT || step == state->wizard_step) return;
    gtk_stack_set_transition_type(GTK_STACK(state->wizard_stack),
        step > state->wizard_step ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
    state->wizard_step = step;

    char step_name[16];
    snprintf(step_name, sizeof(step_name), "step%d", step);
    gtk_stack_set_visible_child_name(GTK_STACK(state->wizard_stack), step_name);
    wizard_update_nav(state);

    if (step == WIZARD_STEP_COUNT - 1) refresh_wizard_test_list(state);
}

static void on_wizard_back_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    goto_wizard_step(state, state->wizard_step - 1);
}

static void on_wizard_next_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    goto_wizard_step(state, state->wizard_step + 1);
}

static void on_refresh_wizard_test_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_wizard_test_list(user_data);
}

/*
 * test_endpoint - dispara un solo request via curl contra
 * localhost:<PORT><path> (PORT de .env, default 8080) y agrega el
 * resultado (codigo HTTP) al panel de Resultados compartido. No es
 * Apache Bench: esto es un smoke test rapido ("responde o no, y con que
 * codigo"), no una prueba de carga -- para eso esta la pestana Carga,
 * que reusa el mismo escaneo de endpoints (scan_routes_file) pero corre
 * ab con las opciones completas (requests/concurrencia/body propio).
 *
 * Los metodos que esperan body (POST/PUT/PATCH) mandan "{}" -- alcanza
 * para confirmar que el endpoint existe y que la autenticacion/ruteo
 * funcionan, aunque la validacion de campos responda 400.
 */
static void test_endpoint(AppState *state, const DiscoveredRoute *r) {
    const char *repo_root = current_repo_root(state);
    char port[16];
    if (!read_env_value(repo_root, "PORT", port, sizeof(port)) || !*port) snprintf(port, sizeof(port), "8080");

    char url[350];
    snprintf(url, sizeof(url), "http://localhost:%s%s", port, r->path);
    char *quoted_url = g_shell_quote(url);

    const char *token = gtk_editable_get_text(GTK_EDITABLE(state->wizard_test_auth_entry));
    int needs_body = strcmp(r->method, "POST") == 0 || strcmp(r->method, "PUT") == 0 || strcmp(r->method, "PATCH") == 0;

    char cmd[1300];
    size_t pos = (size_t)snprintf(cmd, sizeof(cmd), "curl -s -o /dev/null -w '%%{http_code}' -X %s", r->method);
    if (r->requires_auth && *token) {
        char header[600];
        snprintf(header, sizeof(header), "Authorization: Bearer %s", token);
        char *quoted_header = g_shell_quote(header);
        pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, " -H %s", quoted_header);
        g_free(quoted_header);
    }
    if (needs_body) pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, " -H 'Content-Type: application/json' -d '{}'");
    snprintf(cmd + pos, sizeof(cmd) - pos, " %s", quoted_url);
    g_free(quoted_url);

    char header_line[350];
    snprintf(header_line, sizeof(header_line), "--- probando %s %s ---", r->method, r->path);
    append_log(state, header_line);
    pump_gtk_events();

    FILE *proc = popen(cmd, "r");
    if (!proc) {
        append_log(state, "No se pudo lanzar curl (revisa que este instalado).");
        return;
    }
    char status[16] = {0};
    size_t got = fread(status, 1, sizeof(status) - 1, proc);
    (void)got;
    pclose(proc);

    char result_line[350];
    snprintf(result_line, sizeof(result_line), "%s %s -> HTTP %s", r->method, r->path, status[0] ? status : "?  (sin respuesta -- esta corriendo el backend?)");
    append_log(state, result_line);
}

static void on_wizard_probe_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "route-index"));
    if (idx < 0 || idx >= state->load_endpoint_count) return;
    test_endpoint(state, &state->load_endpoints[idx]);
}

/* on_wizard_up_clicked - "docker compose up -d" en la raiz del repo, para
   tener el backend corriendo antes de probar endpoints. Si "Compilar
   proyecto (Docker)" (paso Tablas) no se corrio todavia, compose lo
   construye solo -- este boton no asume que ya este armado. */
static void on_wizard_up_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    const char *repo_root = current_repo_root(state);

    append_log(state, "--- docker compose up -d ---");
    pump_gtk_events();

    char *quoted_root = g_shell_quote(repo_root);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cd %s && docker compose up -d 2>&1", quoted_root);
    g_free(quoted_root);

    int rc = run_streaming_command(cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
    append_log(state, rc == 0 ? "--- servicios arriba ---" : "--- fallo al levantar los servicios (ver arriba) ---");
}

static GtkWidget *build_tables_tab(AppState *state) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    /* Wizard: cada seccion (Conexion/Proyecto/.env/Esquema/Tablas/Probar
       endpoints) es un paso de un GtkStack en vez de un frame mas
       apilado con los demas -- de las ~30 controles de las 6 secciones,
       solo se ve una seccion a la vez, con Atras/Siguiente para navegar.
       "Resultados" (mas abajo, fuera del stack) queda siempre visible en
       cualquier paso. */
    state->wizard_step_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(state->wizard_step_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(state->wizard_step_label), 0.0);
    gtk_box_append(GTK_BOX(root), state->wizard_step_label);

    state->wizard_stack = gtk_stack_new();
    gtk_stack_set_transition_duration(GTK_STACK(state->wizard_stack), 150);
    gtk_box_append(GTK_BOX(root), state->wizard_stack);

    /* --- Paso 1: Conexion --- */
    GtkWidget *conn_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Conexion", &conn_box), "step0");

    state->db_url_entry = gtk_entry_new();
    const char *env_url = getenv("DATABASE_URL");
    if (env_url) gtk_editable_set_text(GTK_EDITABLE(state->db_url_entry), env_url);
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->db_url_entry), "postgresql://usuario:pass@host:5432/base");
    GtkWidget *connect_btn = gtk_button_new_with_label("Conectar");
    gtk_box_append(GTK_BOX(conn_box), row_box("DATABASE_URL:", state->db_url_entry, connect_btn));

    /* Listar bases: para cuando no se sabe (o no se quiere escribir a
       mano) el nombre exacto de la base - usa el host/usuario/contrasena
       de DATABASE_URL de arriba, dbname se ignora para este paso (ver
       on_list_databases_clicked). Elegir una fila arma la DATABASE_URL
       real y conecta de una vez (on_database_row_activated). halign
       START para que no ocupe todo el ancho de la fila como una barra
       -- es una accion secundaria, no la primaria de la seccion. */
    GtkWidget *list_db_btn = gtk_button_new_with_label("Listar bases del servidor");
    gtk_widget_set_halign(list_db_btn, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(conn_box), list_db_btn);

    state->database_listbox = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(state->database_listbox), TRUE);
    GtkWidget *databases_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(databases_scroll), state->database_listbox);
    gtk_widget_set_size_request(databases_scroll, -1, 90);
    gtk_box_append(GTK_BOX(conn_box), databases_scroll);

    /* Postgres de prueba: alternativa a escribir una DATABASE_URL externa
       a mano, para probar dbfiller sin depender de una base real (ver
       ../testdb.h). No reemplaza el campo de arriba, solo lo precarga.
       Label y botones acoplados en una sola fila en vez de label arriba
       + fila de botones abajo. */
    GtkWidget *testdb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *testdb_label = gtk_label_new("Postgres de prueba:");
    gtk_widget_set_size_request(testdb_label, 130, -1);
    gtk_label_set_xalign(GTK_LABEL(testdb_label), 0.0);
    GtkWidget *testdb_start_btn = gtk_button_new_with_label("Levantar");
    GtkWidget *testdb_stop_btn = gtk_button_new_with_label("Detener");
    gtk_box_append(GTK_BOX(testdb_row), testdb_label);
    gtk_box_append(GTK_BOX(testdb_row), testdb_start_btn);
    gtk_box_append(GTK_BOX(testdb_row), testdb_stop_btn);
    gtk_box_append(GTK_BOX(conn_box), testdb_row);

    state->status_label = gtk_label_new("Estado: sin conectar");
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0);
    gtk_box_append(GTK_BOX(conn_box), state->status_label);

    /* --- Paso 2: Proyecto --- */
    GtkWidget *project_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Proyecto", &project_box), "step1");

    state->repo_root_entry = gtk_entry_new();
    char default_root[PATH_MAX];
    default_repo_root(default_root, sizeof(default_root));
    gtk_editable_set_text(GTK_EDITABLE(state->repo_root_entry), default_root);
    GtkWidget *folder_btn = gtk_button_new_with_label("Elegir carpeta...");
    gtk_box_append(GTK_BOX(project_box), row_box("Raiz del repo:", state->repo_root_entry, folder_btn));

    GtkWidget *scaffold_btn = gtk_button_new_with_label("Crear proyecto nuevo (boilerplate)...");
    gtk_widget_set_halign(scaffold_btn, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(project_box), scaffold_btn);

    /* --- Paso 3: Variables de entorno (.env) --- */
    GtkWidget *env_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Variables de entorno (.env)", &env_box), "step2");

    /* USER y DB acoplados en una sola fila (los dos son identificadores
       cortos); PASSWORD y JWT_SECRET quedan en su propia fila cada uno,
       son mas largos y sensibles (ocultos, ver set_visibility). */
    state->env_user_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->env_user_entry), "POSTGRES_USER");
    gtk_widget_set_hexpand(state->env_user_entry, TRUE);
    state->env_db_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->env_db_entry), "POSTGRES_DB");
    gtk_widget_set_hexpand(state->env_db_entry, TRUE);
    GtkWidget *user_db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(user_db_row), gtk_label_new("USER:"));
    gtk_box_append(GTK_BOX(user_db_row), state->env_user_entry);
    gtk_box_append(GTK_BOX(user_db_row), gtk_label_new("DB:"));
    gtk_box_append(GTK_BOX(user_db_row), state->env_db_entry);
    gtk_box_append(GTK_BOX(env_box), user_db_row);

    state->env_password_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(state->env_password_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->env_password_entry), "POSTGRES_PASSWORD");
    gtk_box_append(GTK_BOX(env_box), row_box("PASSWORD:", state->env_password_entry, NULL));

    state->env_jwt_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(state->env_jwt_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->env_jwt_entry), "JWT_SECRET");
    gtk_box_append(GTK_BOX(env_box), row_box("JWT_SECRET:", state->env_jwt_entry, NULL));

    /* Checkbox acoplado a la misma fila que los botones de accion (antes
       ocupaba su propia linea) - hexpand para empujar los dos botones
       (en su propia sub-fila de ancho parejo) hacia la derecha. */
    state->env_overwrite_check = gtk_check_button_new_with_label("Sobreescribir .env existente");
    gtk_widget_set_hexpand(state->env_overwrite_check, TRUE);
    GtkWidget *env_actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *env_buttons = button_row();
    GtkWidget *env_defaults_btn = gtk_button_new_with_label("Generar valores por defecto");
    GtkWidget *env_save_btn = gtk_button_new_with_label("Guardar .env");
    gtk_widget_add_css_class(env_save_btn, "suggested-action");
    gtk_box_append(GTK_BOX(env_buttons), env_defaults_btn);
    gtk_box_append(GTK_BOX(env_buttons), env_save_btn);
    gtk_box_append(GTK_BOX(env_actions_row), state->env_overwrite_check);
    gtk_box_append(GTK_BOX(env_actions_row), env_buttons);
    gtk_box_append(GTK_BOX(env_box), env_actions_row);

    /* --- Paso 4: Esquema --- */
    /* Antes era una pestana aparte, separada de donde se conecta/
       configura la base -- fusionada aca porque cargar un esquema es
       parte del mismo flujo de "levantar y configurar el servicio" (ver
       apply_schema_file: aplica contra la MISMA DATABASE_URL de arriba,
       sea "Postgres de prueba", una base externa, o la que sea, nunca
       contra un servicio de docker-compose asumido a ciegas). */
    GtkWidget *schema_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Esquema", &schema_box), "step3");

    /* "Cargar esquema .sql..." acoplado a la misma fila que el titulo de
       la lista de abajo, en vez de su propia linea + una descripcion de
       dos renglones (el frame ya se llama "Esquema" y la pestana
       Conexion ya explica DATABASE_URL, no hace falta repetirlo aca). */
    GtkWidget *schema_list_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *schema_list_label = gtk_label_new("db/init/:");
    gtk_label_set_xalign(GTK_LABEL(schema_list_label), 0.0);
    gtk_widget_set_hexpand(schema_list_label, TRUE);
    GtkWidget *load_schema_btn = gtk_button_new_with_label("Cargar .sql...");
    GtkWidget *refresh_schema_btn = gtk_button_new_with_label("Refrescar");
    gtk_box_append(GTK_BOX(schema_list_row), schema_list_label);
    gtk_box_append(GTK_BOX(schema_list_row), load_schema_btn);
    gtk_box_append(GTK_BOX(schema_list_row), refresh_schema_btn);
    gtk_box_append(GTK_BOX(schema_box), schema_list_row);

    state->schema_listbox = gtk_list_box_new();
    GtkWidget *schema_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(schema_scroll), state->schema_listbox);
    gtk_widget_set_size_request(schema_scroll, -1, 120);
    gtk_box_append(GTK_BOX(schema_box), schema_scroll);

    /* --- Paso 5: Tablas --- */
    GtkWidget *tables_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Tablas ('public')", &tables_box), "step4");

    /* --force acoplado a la misma fila que Seleccionar todas/Ninguna en
       vez de su propia linea. */
    state->force_check = gtk_check_button_new_with_label("Sobreescribir (--force)");
    gtk_widget_set_hexpand(state->force_check, TRUE);
    GtkWidget *select_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *select_buttons = button_row();
    GtkWidget *select_all_btn = gtk_button_new_with_label("Seleccionar todas");
    GtkWidget *select_none_btn = gtk_button_new_with_label("Ninguna");
    gtk_box_append(GTK_BOX(select_buttons), select_all_btn);
    gtk_box_append(GTK_BOX(select_buttons), select_none_btn);
    gtk_box_append(GTK_BOX(select_row), state->force_check);
    gtk_box_append(GTK_BOX(select_row), select_buttons);
    gtk_box_append(GTK_BOX(tables_box), select_row);

    state->table_listbox = gtk_list_box_new();
    GtkWidget *tables_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tables_scroll), state->table_listbox);
    gtk_widget_set_size_request(tables_scroll, -1, 160);
    gtk_box_append(GTK_BOX(tables_box), tables_scroll);

    GtkWidget *actions_row = button_row();
    GtkWidget *generate_btn = gtk_button_new_with_label("Generar seleccionadas");
    gtk_widget_add_css_class(generate_btn, "suggested-action");
    GtkWidget *build_btn = gtk_button_new_with_label("Compilar proyecto (Docker)");
    gtk_box_append(GTK_BOX(actions_row), generate_btn);
    gtk_box_append(GTK_BOX(actions_row), build_btn);
    gtk_box_append(GTK_BOX(tables_box), actions_row);

    /* --- Paso 6: Probar endpoints --- */
    /* Ultimo paso del wizard: lista de lo ya registrado en
       routes/index.h (mismo escaneo que la pestana Carga, ver
       scan_routes_file) con un boton "Probar" por fila -- un smoke test
       de un solo request (ver test_endpoint), no una prueba de carga.
       Se repuebla sola al entrar a este paso (goto_wizard_step). */
    GtkWidget *test_box;
    gtk_stack_add_named(GTK_STACK(state->wizard_stack), section_frame("Probar endpoints", &test_box), "step5");

    GtkWidget *test_top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *up_btn = gtk_button_new_with_label("Levantar (docker compose up -d)");
    GtkWidget *refresh_test_btn = gtk_button_new_with_label("Refrescar lista");
    gtk_box_append(GTK_BOX(test_top_row), up_btn);
    gtk_box_append(GTK_BOX(test_top_row), refresh_test_btn);
    gtk_box_append(GTK_BOX(test_box), test_top_row);

    state->wizard_test_auth_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->wizard_test_auth_entry),
                                    "token JWT, sin \"Bearer \" (para endpoints marcados (JWT))");
    gtk_box_append(GTK_BOX(test_box), row_box("Authorization:", state->wizard_test_auth_entry, NULL));

    state->wizard_test_listbox = gtk_list_box_new();
    GtkWidget *test_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(test_scroll), state->wizard_test_listbox);
    gtk_widget_set_size_request(test_scroll, -1, 200);
    gtk_box_append(GTK_BOX(test_box), test_scroll);

    /* --- Navegacion del wizard --- */
    GtkWidget *wizard_nav_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->wizard_back_btn = gtk_button_new_with_label("Atras");
    state->wizard_next_btn = gtk_button_new_with_label("Siguiente");
    gtk_widget_set_hexpand(state->wizard_back_btn, TRUE);
    gtk_widget_set_hexpand(state->wizard_next_btn, TRUE);
    gtk_box_append(GTK_BOX(wizard_nav_row), state->wizard_back_btn);
    gtk_box_append(GTK_BOX(wizard_nav_row), state->wizard_next_btn);
    gtk_box_append(GTK_BOX(root), wizard_nav_row);

    /* --- Resultados (siempre visible, fuera del stack) --- */
    GtkWidget *log_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *log_label = gtk_label_new("Resultados:");
    gtk_label_set_xalign(GTK_LABEL(log_label), 0.0);
    gtk_widget_set_hexpand(log_label, TRUE);
    GtkWidget *copy_logs_btn = gtk_button_new_with_label("Copiar logs");
    gtk_box_append(GTK_BOX(log_row), log_label);
    gtk_box_append(GTK_BOX(log_row), copy_logs_btn);
    gtk_box_append(GTK_BOX(root), log_row);

    state->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->log_view), TRUE);
    state->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->log_view));
    GtkWidget *log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll), state->log_view);
    gtk_widget_set_vexpand(log_scroll, TRUE);
    gtk_box_append(GTK_BOX(root), log_scroll);

    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), state);
    g_signal_connect(list_db_btn, "clicked", G_CALLBACK(on_list_databases_clicked), state);
    g_signal_connect(state->database_listbox, "row-activated", G_CALLBACK(on_database_row_activated), state);
    g_signal_connect(testdb_start_btn, "clicked", G_CALLBACK(on_testdb_start_clicked), state);
    g_signal_connect(testdb_stop_btn, "clicked", G_CALLBACK(on_testdb_stop_clicked), state);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_choose_folder_clicked), state);
    g_signal_connect(scaffold_btn, "clicked", G_CALLBACK(on_scaffold_clicked), state);
    g_signal_connect(env_defaults_btn, "clicked", G_CALLBACK(on_generate_env_defaults_clicked), state);
    g_signal_connect(env_save_btn, "clicked", G_CALLBACK(on_save_env_clicked), state);
    g_signal_connect(load_schema_btn, "clicked", G_CALLBACK(on_load_schema_clicked), state);
    g_signal_connect(refresh_schema_btn, "clicked", G_CALLBACK(on_refresh_schema_list_clicked), state);
    g_signal_connect(select_all_btn, "clicked", G_CALLBACK(on_select_all_clicked), state);
    g_signal_connect(select_none_btn, "clicked", G_CALLBACK(on_select_none_clicked), state);
    g_signal_connect(generate_btn, "clicked", G_CALLBACK(on_generate_clicked), state);
    g_signal_connect(build_btn, "clicked", G_CALLBACK(on_build_clicked), state);
    g_signal_connect(copy_logs_btn, "clicked", G_CALLBACK(on_copy_logs_clicked), state);
    g_signal_connect(up_btn, "clicked", G_CALLBACK(on_wizard_up_clicked), state);
    g_signal_connect(refresh_test_btn, "clicked", G_CALLBACK(on_refresh_wizard_test_clicked), state);
    g_signal_connect(state->wizard_back_btn, "clicked", G_CALLBACK(on_wizard_back_clicked), state);
    g_signal_connect(state->wizard_next_btn, "clicked", G_CALLBACK(on_wizard_next_clicked), state);

    state->wizard_step = 0;
    wizard_update_nav(state);

    return root;
}

/* ======================================================================
 * Esquema - funciones de soporte del paso "Esquema" del wizard (arriba)
 * ====================================================================== */

/* next_schema_prefix - siguiente prefijo numerico libre para un script
   nuevo en <repo_root>/db/init/ (escanea NN_*.sql existentes, usa
   max+1, nunca menor a 2 para no chocar con 01_auth_schema.sql). */
static int next_schema_prefix(const char *repo_root) {
    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", repo_root);

    DIR *dir = opendir(dir_path);
    if (!dir) return 2;

    int max_prefix = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int n;
        if (sscanf(entry->d_name, "%2d_", &n) == 1 && n > max_prefix) max_prefix = n;
    }
    closedir(dir);
    return max_prefix + 1;
}

/* apply_schema_file - aplica db/init/<filename> (ya presente ahi, ver
 * on_schema_file_chosen) contra la MISMA base a la que apunta
 * DATABASE_URL ahora mismo -- no contra "el servicio db del
 * docker-compose.yml de este repo" como hacia antes. Ese era un bug real:
 * aplicaba siempre al 'db' de este repo via `docker compose exec`,
 * ignorando por completo si el usuario habia elegido otra base con
 * "Listar bases del servidor" o con "Postgres de prueba" -- el esquema
 * podia terminar en un servidor distinto al que se estaba mirando en el
 * resto de este mismo panel, o directamente fallar si ese 'db' no estaba
 * corriendo.
 *
 * El host no tiene el binario `psql` instalado, asi que se usa un
 * contenedor `postgres:16-alpine` desechable (`docker run --rm -i`) solo
 * para tener uno a mano, con --network=host para que pueda alcanzar
 * "localhost:<puerto>" (Postgres de prueba) o cualquier host/IP externo
 * exactamente igual que lo alcanzaria un proceso nativo del host. El
 * archivo se le manda por stdin (no -f): asi no hace falta que el
 * contenedor vea el archivo en su propio filesystem para nada.
 */
static void apply_schema_file(AppState *state, const char *filename) {
    const char *repo_root = current_repo_root(state);
    const char *db_url = current_db_url(state);
    if (!*db_url) {
        append_log(state, "Escribi (o conecta) una DATABASE_URL primero -- el esquema se aplica contra esa base.");
        return;
    }

    char header[300];
    snprintf(header, sizeof(header), "--- aplicando %s ---", filename);
    append_log(state, header);
    pump_gtk_events();

    char sql_path[1200];
    snprintf(sql_path, sizeof(sql_path), "%s/db/init/%s", repo_root, filename);

    char *quoted_url = g_shell_quote(db_url);
    char *quoted_sql_path = g_shell_quote(sql_path);
    char cmd[2400];
    snprintf(cmd, sizeof(cmd),
             "docker run --rm -i --network=host postgres:16-alpine psql %s < %s 2>&1",
             quoted_url, quoted_sql_path);
    g_free(quoted_url);
    g_free(quoted_sql_path);

    int rc = run_streaming_command(cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
    append_log(state, rc == 0 ? "--- esquema aplicado OK ---" : "--- fallo al aplicar el esquema (ver arriba) ---");
    if (rc != 0) return;

    /* Muestra de una vez que tablas quedaron en la base, en el mismo
       panel donde se aplico el esquema -- antes habia que acordarse de
       ir a "Conectar" de nuevo para confirmar que el import funciono. */
    append_log(state, "--- tablas en la base ---");
    char *quoted_url2 = g_shell_quote(db_url);
    char list_cmd[700];
    snprintf(list_cmd, sizeof(list_cmd), "docker run --rm --network=host postgres:16-alpine psql %s -c '\\dt' 2>&1", quoted_url2);
    g_free(quoted_url2);
    run_streaming_command(list_cmd, state->log_buffer, GTK_TEXT_VIEW(state->log_view));
}

static void on_apply_schema_row_clicked(GtkButton *button, gpointer user_data) {
    AppState *state = user_data;
    const char *filename = g_object_get_data(G_OBJECT(button), "schema-filename");
    if (filename) apply_schema_file(state, filename);
}

static int cmp_schema_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* refresh_schema_list - repuebla schema_listbox con cada
   db/init/NN_*.sql de la raiz del repo (excluida 01_auth_schema.sql: es
   infraestructura fija del boilerplate, no "esquema de negocio"), una
   fila por archivo con un boton para volver a aplicarlo sin tener que
   volver a elegirlo del filesystem. */
static void refresh_schema_list(AppState *state) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(state->schema_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(state->schema_listbox), child);
    }

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", current_repo_root(state));

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    char names[256][256];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 256) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".sql") != 0) continue;
        if (strcmp(entry->d_name, "01_auth_schema.sql") == 0) continue;
        snprintf(names[count++], sizeof(names[0]), "%s", entry->d_name);
    }
    closedir(dir);

    qsort(names, (size_t)count, sizeof(names[0]), cmp_schema_names);

    for (int i = 0; i < count; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name_label = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
        gtk_widget_set_hexpand(name_label, TRUE);
        GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar de nuevo");
        g_object_set_data_full(G_OBJECT(apply_btn), "schema-filename", g_strdup(names[i]), g_free);
        g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_schema_row_clicked), state);
        gtk_box_append(GTK_BOX(row), name_label);
        gtk_box_append(GTK_BOX(row), apply_btn);
        gtk_list_box_append(GTK_LIST_BOX(state->schema_listbox), row);
    }

    if (count == 0) {
        GtkWidget *empty = gtk_label_new("(ningun esquema propio cargado todavia)");
        gtk_list_box_append(GTK_LIST_BOX(state->schema_listbox), empty);
    }
}

static void on_refresh_schema_list_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_schema_list(user_data);
}

/* on_schema_file_chosen - copia el .sql elegido a
   db/init/<NN>_<basename> (numerado con next_schema_prefix), lo aplica
   de inmediato al contenedor corriendo, y refresca la lista. */
static void on_schema_file_chosen(GObject *source, GAsyncResult *result, gpointer user_data) {
    AppState *state = user_data;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_object_unref(source);
    if (!file) return;

    char *src_path = g_file_get_path(file);
    g_object_unref(file);
    if (!src_path) return;

    const char *repo_root = current_repo_root(state);

    /* "Raiz del repo" puede no apuntar (todavia) a un checkout real de
       cerver, o apuntar a uno que nunca tuvo db/init/ (un proyecto recien
       creado con "Crear proyecto nuevo" si por algun motivo no lo trajo).
       g_file_set_contents() mas abajo NO crea directorios intermedios -
       sin este paso, falla con un error de GIO poco claro ("Fallo al
       crear el archivo «...», No existe el archivo o el directorio") en
       vez de decir directamente que falta la carpeta. */
    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", repo_root);
    if (g_mkdir_with_parents(dir_path, 0755) != 0) {
        char msg[1100];
        snprintf(msg, sizeof(msg), "No se pudo crear la carpeta '%s': %s (revisa \"Raiz del repo\")",
                 dir_path, g_strerror(errno));
        append_log(state, msg);
        g_free(src_path);
        return;
    }

    /* Si el archivo elegido ya esta adentro de este mismo db/init/ (el
       caso tipico: el usuario vuelve a elegir del selector de archivos un
       .sql que ya se habia importado antes, en vez de usar "Aplicar de
       nuevo" en la lista de abajo), no copiarlo de nuevo con un prefijo
       encima del que ya tiene -- eso es lo que produjo el
       "03_02_archivo.sql" del reporte de este bug: cada vez que se
       repetia el mismo .sql se le agregaba un prefijo mas, sin fin. En
       ese caso alcanza con re-aplicar el archivo que ya esta ahi. */
    char src_dir[1024];
    snprintf(src_dir, sizeof(src_dir), "%s", src_path);
    char *last_slash = strrchr(src_dir, '/');
    if (last_slash) *last_slash = '\0';

    char real_dir_path[PATH_MAX], real_src_dir[PATH_MAX];
    if (realpath(dir_path, real_dir_path) && realpath(src_dir, real_src_dir) &&
        strcmp(real_dir_path, real_src_dir) == 0) {
        const char *existing_base = strrchr(src_path, '/');
        existing_base = existing_base ? existing_base + 1 : src_path;
        char msg[400];
        snprintf(msg, sizeof(msg), "'%s' ya esta en db/init/ -- aplicando sin duplicar.", existing_base);
        append_log(state, msg);
        apply_schema_file(state, existing_base);
        g_free(src_path);
        refresh_schema_list(state);
        return;
    }

    int prefix = next_schema_prefix(repo_root);
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;

    /* Si el nombre de origen ya trae un prefijo numerico propio (p.ej.
       viene de otro proyecto y se llama "02_algo.sql"), se lo saca antes
       de anteponer el prefijo nuevo -- mismo motivo que arriba, evitar
       nombres tipo "03_02_algo.sql". */
    if (isdigit((unsigned char)base[0]) && isdigit((unsigned char)base[1]) && base[2] == '_') base += 3;

    char dest_name[300];
    snprintf(dest_name, sizeof(dest_name), "%02d_%s", prefix, base);
    char dest_path[1200];
    snprintf(dest_path, sizeof(dest_path), "%s/db/init/%s", repo_root, dest_name);

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(src_path, &contents, &length, &error)) {
        char msg[600];
        snprintf(msg, sizeof(msg), "No se pudo leer '%s': %s", src_path, error ? error->message : "?");
        append_log(state, msg);
        if (error) g_error_free(error);
        g_free(src_path);
        return;
    }
    g_free(src_path);

    if (!g_file_set_contents(dest_path, contents, (gssize)length, &error)) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "No se pudo copiar a '%s': %s", dest_path, error ? error->message : "?");
        append_log(state, msg);
        if (error) g_error_free(error);
        g_free(contents);
        return;
    }
    g_free(contents);

    char msg[400];
    snprintf(msg, sizeof(msg), "Copiado a db/init/%s", dest_name);
    append_log(state, msg);

    apply_schema_file(state, dest_name);
    refresh_schema_list(state);
}

static void on_load_schema_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Elegir esquema .sql");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_suffix(filter, "sql");
    gtk_file_filter_set_name(filter, "Archivos SQL (*.sql)");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(state->window), NULL, on_schema_file_chosen, state);
}

/* ======================================================================
 * Pestana Docker (solo monitoreo)
 * ====================================================================== */

/* refresh_docker_stats_panel - solo "docker stats --no-stream", el panel
   mas liviano de los tres (no toca el repo ni el registro de imagenes) -
   es el unico que refresca el polling en vivo (on_docker_live_tick), para
   no relanzar "docker compose ps"/"docker images" cada 2 segundos sin
   necesidad. */
static void refresh_docker_stats_panel(AppState *state) {
    gtk_text_buffer_set_text(state->docker_stats_buffer, "", -1);
    run_streaming_command("docker stats --no-stream 2>&1", state->docker_stats_buffer, GTK_TEXT_VIEW(state->docker_stats_view));
}

/*
 * on_docker_refresh_clicked - corre, en secuencia, "docker compose ps",
 * "docker stats --no-stream" y "docker images", volcando cada uno en su
 * propio panel.
 */
static void on_docker_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    char *quoted_root = g_shell_quote(current_repo_root(state));

    gtk_text_buffer_set_text(state->docker_containers_buffer, "", -1);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd %s && docker compose ps 2>&1", quoted_root);
    run_streaming_command(cmd, state->docker_containers_buffer, GTK_TEXT_VIEW(state->docker_containers_view));

    refresh_docker_stats_panel(state);

    gtk_text_buffer_set_text(state->docker_images_buffer, "", -1);
    run_streaming_command("docker images 2>&1", state->docker_images_buffer, GTK_TEXT_VIEW(state->docker_images_view));

    g_free(quoted_root);
}

/* on_docker_live_tick - callback de g_timeout_add_seconds mientras
   "Actualizar en vivo" esta activo. Devolver G_SOURCE_CONTINUE lo
   reprograma para la proxima tick; el timer se cancela explicitamente en
   on_docker_live_toggled (destildar el check) o on_app_shutdown (cerrar
   la ventana), nunca devolviendo G_SOURCE_REMOVE desde aca. */
static gboolean on_docker_live_tick(gpointer user_data) {
    refresh_docker_stats_panel(user_data);
    return G_SOURCE_CONTINUE;
}

/*
 * on_docker_live_toggled - prende/apaga el polling de "docker stats" cada
 * 2 segundos. Deliberadamente no reemplaza el patron "sin hilo de fondo"
 * del resto de la GUI (ver comentario al principio del archivo): un
 * g_timeout de GLib corre en el mismo hilo principal, entre iteraciones
 * del loop de eventos, no en un hilo aparte - sigue sin haber
 * sincronizacion que pensar. Es un timer explicito, prendido por el
 * usuario (a diferencia de un polling siempre-activo), asi que no
 * contradice el motivo original de no tener timers automaticos.
 */
static void on_docker_live_toggled(GtkCheckButton *check, gpointer user_data) {
    AppState *state = user_data;
    if (gtk_check_button_get_active(check)) {
        if (state->docker_live_timer_id == 0) {
            refresh_docker_stats_panel(state); /* primer refresco inmediato, no esperar 2s */
            state->docker_live_timer_id = g_timeout_add_seconds(2, on_docker_live_tick, state);
        }
    } else if (state->docker_live_timer_id != 0) {
        g_source_remove(state->docker_live_timer_id);
        state->docker_live_timer_id = 0;
    }
}

static GtkWidget *build_docker_tab(AppState *state) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    GtkWidget *actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refrescar");
    gtk_widget_add_css_class(refresh_btn, "suggested-action");
    state->docker_live_check = gtk_check_button_new_with_label("Actualizar stats en vivo (cada 2s)");
    gtk_box_append(GTK_BOX(actions_row), refresh_btn);
    gtk_box_append(GTK_BOX(actions_row), state->docker_live_check);
    gtk_box_append(GTK_BOX(root), actions_row);

    gtk_box_append(GTK_BOX(root), labeled_output("Containers (docker compose ps):", 100, &state->docker_containers_buffer, &state->docker_containers_view));
    gtk_box_append(GTK_BOX(root), labeled_output("Stats (docker stats --no-stream):", 100, &state->docker_stats_buffer, &state->docker_stats_view));
    gtk_box_append(GTK_BOX(root), labeled_output("Imagenes (docker images):", 100, &state->docker_images_buffer, &state->docker_images_view));

    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_docker_refresh_clicked), state);
    g_signal_connect(state->docker_live_check, "toggled", G_CALLBACK(on_docker_live_toggled), state);

    return root;
}

/* ======================================================================
 * Pestana Carga (Apache Bench)
 * ====================================================================== */

/*
 * scan_routes_file - busca llamadas get()/post()/put()/patch()/del() y
 * sus variantes _auth() en <repo_root>/routes/index.h (ver
 * utils/http/router.h) y llena out con una DiscoveredRoute por cada una.
 *
 * No es un parser de C de verdad -- no hace falta: esas lineas siempre
 * tienen la forma exacta "<verbo>(\"<path>\", <handler>);" (a mano o
 * generadas por repo_patch.c), asi que alcanza con reconocer el prefijo
 * del verbo al principio de la linea (ignorando espacios/tabs) y leer el
 * primer string entre comillas. Las lineas de comentario (empiezan con
 * '*' o "//" despues de recortar espacios) se saltean explicitamente,
 * porque la descripcion de este mismo archivo menciona "get()/post()"
 * como ejemplo de texto.
 *
 * Retorna la cantidad de rutas encontradas (0 si routes/index.h no
 * existe o no matcheo ninguna).
 */
static int scan_routes_file(const char *repo_root, DiscoveredRoute *out, int max_routes) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/routes/index.h", repo_root);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    static const struct { const char *prefix; const char *method; int auth; } verbs[] = {
        {"get_auth(", "GET", 1}, {"post_auth(", "POST", 1}, {"put_auth(", "PUT", 1},
        {"patch_auth(", "PATCH", 1}, {"del_auth(", "DELETE", 1},
        {"get(", "GET", 0}, {"post(", "POST", 0}, {"put(", "PUT", 0},
        {"patch(", "PATCH", 0}, {"del(", "DELETE", 0},
    };

    char line[512];
    int count = 0;
    while (count < max_routes && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '*' || (p[0] == '/' && p[1] == '/')) continue;

        for (size_t v = 0; v < sizeof(verbs) / sizeof(verbs[0]); v++) {
            size_t plen = strlen(verbs[v].prefix);
            if (strncmp(p, verbs[v].prefix, plen) != 0) continue;

            char *quote1 = strchr(p + plen, '"');
            if (!quote1) break;
            char *quote2 = strchr(quote1 + 1, '"');
            if (!quote2) break;

            size_t path_len = (size_t)(quote2 - quote1 - 1);
            DiscoveredRoute *r = &out[count];
            if (path_len >= sizeof(r->path)) path_len = sizeof(r->path) - 1;

            snprintf(r->method, sizeof(r->method), "%s", verbs[v].method);
            memcpy(r->path, quote1 + 1, path_len);
            r->path[path_len] = '\0';
            r->requires_auth = verbs[v].auth;
            count++;
            break;
        }
    }
    fclose(f);
    return count;
}

/* refresh_load_endpoints - re-escanea routes/index.h de "Raiz del repo"
   (pestana Tablas) y repuebla el desplegable de endpoints. Se llama al
   construir la pestana y con el boton "Refrescar endpoints" -- hace
   falta re-escanear despues de generar tablas nuevas o de cambiar de
   proyecto, ninguno de los cuales dispara esto solo. */
static void refresh_load_endpoints(AppState *state) {
    const char *repo_root = current_repo_root(state);
    state->load_endpoint_count = scan_routes_file(repo_root, state->load_endpoints, MAX_DISCOVERED_ROUTES);

    GtkStringList *model = gtk_string_list_new(NULL);
    if (state->load_endpoint_count == 0) {
        gtk_string_list_append(model, "(sin endpoints -- conecta/genera tablas o revisa \"Raiz del repo\")");
    } else {
        for (int i = 0; i < state->load_endpoint_count; i++) {
            const DiscoveredRoute *r = &state->load_endpoints[i];
            char label[300];
            snprintf(label, sizeof(label), "%s %s%s", r->method, r->path, r->requires_auth ? "  (requiere JWT)" : "");
            gtk_string_list_append(model, label);
        }
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(state->load_endpoint_dropdown), G_LIST_MODEL(model));
    g_object_unref(model);
}

static void on_refresh_load_endpoints_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_load_endpoints(user_data);
}

/* on_load_endpoint_selected - al elegir un endpoint del desplegable,
   precarga Path y Metodo con lo que ese endpoint realmente espera (en
   vez de tener que saberlo/escribirlo a mano) -- si requiere JWT, el
   propio texto de la opcion elegida ya lo aclara ("requiere JWT"), asi
   que solo queda pegar el token en "Authorization" antes de correr. */
static void on_load_endpoint_selected(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    AppState *state = user_data;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (idx == GTK_INVALID_LIST_POSITION || (int)idx >= state->load_endpoint_count) return;

    const DiscoveredRoute *r = &state->load_endpoints[idx];
    gtk_editable_set_text(GTK_EDITABLE(state->load_path_entry), r->path);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->load_method_dropdown), strcmp(r->method, "POST") == 0 ? 1 : 0);
}

/*
 * on_run_load_test_clicked - arma y corre un comando "ab" con las
 * opciones del formulario. GET no manda body; POST escribe el body a un
 * archivo temporal (ab solo acepta el body de un POST desde un archivo,
 * via -p). El metodo lo decide 'ab' segun si se paso -p, no hace falta
 * un flag de metodo aparte -- por eso PUT/DELETE no estan soportados
 * aca (ab no tiene una forma portable de pedir un metodo arbitrario en
 * todas las versiones).
 */
static void on_run_load_test_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;
    const char *repo_root = current_repo_root(state);

    char port[16];
    if (!read_env_value(repo_root, "PORT", port, sizeof(port)) || !*port) snprintf(port, sizeof(port), "8080");

    const char *path = gtk_editable_get_text(GTK_EDITABLE(state->load_path_entry));
    if (!*path) path = "/healthz";

    guint method_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->load_method_dropdown));
    int is_post = (method_idx == 1);

    int n = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->load_requests_spin));
    int c = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->load_concurrency_spin));
    int keep_alive = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->load_keepalive_check));
    const char *auth = gtk_editable_get_text(GTK_EDITABLE(state->load_auth_entry));

    char *quoted_header = NULL;
    if (*auth) {
        char header[600];
        snprintf(header, sizeof(header), "Authorization: Bearer %s", auth);
        quoted_header = g_shell_quote(header);
    }

    char *quoted_body_path = NULL;
    if (is_post) {
        GtkTextBuffer *body_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->load_body_view));
        GtkTextIter bstart, bend;
        gtk_text_buffer_get_bounds(body_buf, &bstart, &bend);
        char *body_text = gtk_text_buffer_get_text(body_buf, &bstart, &bend, FALSE);

        char body_path[512];
        snprintf(body_path, sizeof(body_path), "%s/dbfiller_ab_body.json", g_get_tmp_dir());
        GError *error = NULL;
        if (!g_file_set_contents(body_path, body_text, -1, &error)) {
            char msg[600];
            snprintf(msg, sizeof(msg), "No se pudo escribir el body temporal: %s", error ? error->message : "?");
            append_line_to_buffer(state->load_output_buffer, GTK_TEXT_VIEW(state->load_output_view), msg);
            if (error) g_error_free(error);
            g_free(body_text);
            g_free(quoted_header);
            return;
        }
        g_free(body_text);
        quoted_body_path = g_shell_quote(body_path);
    }

    char url[300];
    snprintf(url, sizeof(url), "http://localhost:%s%s", port, path);
    char *quoted_url = g_shell_quote(url);

    char cmd[2200];
    size_t pos = 0;
    pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, "ab -n %d -c %d", n, c);
    if (keep_alive) pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, " -k");
    if (quoted_header) pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, " -H %s", quoted_header);
    if (quoted_body_path) pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, " -p %s -T application/json", quoted_body_path);
    snprintf(cmd + pos, sizeof(cmd) - pos, " %s 2>&1", quoted_url);

    g_free(quoted_header);
    g_free(quoted_body_path);
    g_free(quoted_url);

    gtk_text_buffer_set_text(state->load_output_buffer, "", -1);
    append_line_to_buffer(state->load_output_buffer, GTK_TEXT_VIEW(state->load_output_view), cmd);
    pump_gtk_events();

    run_streaming_command(cmd, state->load_output_buffer, GTK_TEXT_VIEW(state->load_output_view));
}

static GtkWidget *build_load_tab(AppState *state) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    /* Endpoint: desplegable con lo ya registrado en routes/index.h del
       proyecto en "Raiz del repo" (pestana Tablas) -- elegir uno precarga
       Path/Metodo (on_load_endpoint_selected), en vez de tener que saber
       de memoria que endpoints existen y escribirlos a mano. "Refrescar"
       hace falta despues de generar tablas nuevas o cambiar de proyecto. */
    state->load_endpoint_dropdown = gtk_drop_down_new(NULL, NULL);
    GtkWidget *refresh_endpoints_btn = gtk_button_new_with_label("Refrescar");
    gtk_box_append(GTK_BOX(root), row_box("Endpoint:", state->load_endpoint_dropdown, refresh_endpoints_btn));

    state->load_path_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(state->load_path_entry), "/healthz");
    gtk_widget_set_hexpand(state->load_path_entry, TRUE);
    const char *methods[] = {"GET", "POST", NULL};
    state->load_method_dropdown = gtk_drop_down_new_from_strings(methods);
    GtkWidget *path_method_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *path_label = gtk_label_new("Path:");
    gtk_widget_set_size_request(path_label, 120, -1);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0);
    gtk_box_append(GTK_BOX(path_method_row), path_label);
    gtk_box_append(GTK_BOX(path_method_row), state->load_path_entry);
    gtk_box_append(GTK_BOX(path_method_row), state->load_method_dropdown);
    gtk_box_append(GTK_BOX(root), path_method_row);

    state->load_auth_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->load_auth_entry), "token JWT, sin \"Bearer \" (opcional)");
    gtk_widget_set_hexpand(state->load_auth_entry, TRUE);
    state->load_keepalive_check = gtk_check_button_new_with_label("Keep-alive");
    GtkWidget *auth_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *auth_label = gtk_label_new("Authorization:");
    gtk_widget_set_size_request(auth_label, 120, -1);
    gtk_label_set_xalign(GTK_LABEL(auth_label), 0.0);
    gtk_box_append(GTK_BOX(auth_row), auth_label);
    gtk_box_append(GTK_BOX(auth_row), state->load_auth_entry);
    gtk_box_append(GTK_BOX(auth_row), state->load_keepalive_check);
    gtk_box_append(GTK_BOX(root), auth_row);

    GtkWidget *body_label = gtk_label_new("Body (solo POST, JSON):");
    gtk_label_set_xalign(GTK_LABEL(body_label), 0.0);
    gtk_box_append(GTK_BOX(root), body_label);
    state->load_body_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(state->load_body_view), TRUE);
    GtkWidget *body_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(body_scroll), state->load_body_view);
    gtk_widget_set_size_request(body_scroll, -1, 70);
    gtk_box_append(GTK_BOX(root), body_scroll);

    GtkWidget *nc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->load_requests_spin = gtk_spin_button_new_with_range(1, 10000000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->load_requests_spin), 10000);
    state->load_concurrency_spin = gtk_spin_button_new_with_range(1, 10000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->load_concurrency_spin), 100);
    gtk_box_append(GTK_BOX(nc_row), gtk_label_new("Requests:"));
    gtk_box_append(GTK_BOX(nc_row), state->load_requests_spin);
    gtk_box_append(GTK_BOX(nc_row), gtk_label_new("Concurrencia:"));
    gtk_box_append(GTK_BOX(nc_row), state->load_concurrency_spin);
    gtk_box_append(GTK_BOX(root), nc_row);

    GtkWidget *run_btn = gtk_button_new_with_label("Correr prueba");
    gtk_widget_add_css_class(run_btn, "suggested-action");
    gtk_box_append(GTK_BOX(root), run_btn);

    GtkWidget *output_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *output_label = gtk_label_new("Resultados:");
    gtk_label_set_xalign(GTK_LABEL(output_label), 0.0);
    gtk_widget_set_hexpand(output_label, TRUE);
    GtkWidget *copy_load_logs_btn = gtk_button_new_with_label("Copiar logs");
    gtk_box_append(GTK_BOX(output_row), output_label);
    gtk_box_append(GTK_BOX(output_row), copy_load_logs_btn);
    gtk_box_append(GTK_BOX(root), output_row);

    GtkWidget *output = labeled_output(NULL, 220, &state->load_output_buffer, &state->load_output_view);
    gtk_box_append(GTK_BOX(root), output);

    g_signal_connect(run_btn, "clicked", G_CALLBACK(on_run_load_test_clicked), state);
    g_signal_connect(copy_load_logs_btn, "clicked", G_CALLBACK(on_copy_load_logs_clicked), state);
    g_signal_connect(refresh_endpoints_btn, "clicked", G_CALLBACK(on_refresh_load_endpoints_clicked), state);
    g_signal_connect(state->load_endpoint_dropdown, "notify::selected", G_CALLBACK(on_load_endpoint_selected), state);

    refresh_load_endpoints(state);

    return root;
}

/* ======================================================================
 * Ventana principal
 * ====================================================================== */

static void on_app_shutdown(GtkApplication *app, gpointer user_data) {
    (void)app;
    AppState *state = user_data;
    if (state->docker_live_timer_id != 0) g_source_remove(state->docker_live_timer_id);
    if (state->conn) PQfinish(state->conn);
    g_free(state);
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AppState *state = g_new0(AppState, 1);
    state->app = app;

    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "dbfiller -- generador de endpoints CRUD");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 760, 820);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_tables_tab(state), gtk_label_new("Tablas"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_docker_tab(state), gtk_label_new("Docker"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_load_tab(state), gtk_label_new("Carga"));
    gtk_window_set_child(GTK_WINDOW(state->window), notebook);

    refresh_schema_list(state);

    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), state);

    gtk_window_present(GTK_WINDOW(state->window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("dev.cerver.dbfiller", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
