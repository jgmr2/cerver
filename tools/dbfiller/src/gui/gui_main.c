/*
 * gui/gui_main.c - interfaz grafica de dbfiller (GTK4)
 *
 * DESCRIPCION
 *     Front-end alternativo al CLI (../main.c): mismo flujo (conectar a
 *     Postgres, listar tablas del schema 'public', generar CRUD para
 *     las que se elijan), pero con formulario en vez de flags. Ninguna
 *     logica vive aca - todo pasa por dbfiller_generate_table()
 *     (../generate.h), la misma funcion que usa el CLI, para que ambos
 *     front-ends se comporten identico y no haya dos implementaciones
 *     del mismo flujo para mantener sincronizadas.
 *
 *     Simplificacion aceptada a proposito: las llamadas a Postgres y la
 *     escritura de archivos corren en el hilo principal de GTK (sin
 *     hilo de fondo). Para el uso esperado (Postgres local/de
 *     desarrollo, unas pocas tablas por vez) es practicamente
 *     instantaneo; el costo es que la ventana no responde por la
 *     fraccion de segundo que dura cada operacion. Ver README.md.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "../introspect.h"
#include "../generate.h"

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *db_url_entry;
    GtkWidget *repo_root_entry;
    GtkWidget *force_check;
    GtkWidget *status_label;
    GtkWidget *table_listbox;
    GtkTextBuffer *log_buffer;
    GtkWidget *log_view;
    PGconn *conn;
} AppState;

/* append_log - agrega una linea al panel de resultados y hace scroll al
   final, mismo contenido que hoy imprime el CLI por stdout/stderr. */
static void append_log(AppState *state, const char *line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(state->log_buffer, &end);
    gtk_text_buffer_insert(state->log_buffer, &end, line, -1);
    gtk_text_buffer_insert(state->log_buffer, &end, "\n", -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(state->log_buffer);
    gtk_text_buffer_get_end_iter(state->log_buffer, &end);
    gtk_text_buffer_move_mark(state->log_buffer, mark, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(state->log_view), mark, 0.0, FALSE, 0.0, 1.0);
}

static void set_status(AppState *state, const char *text) {
    gtk_label_set_text(GTK_LABEL(state->status_label), text);
}

/* pump_gtk_events - procesa los eventos pendientes del loop de GTK sin
   bloquear. Se usa entre pasos de una operacion sincronica larga
   (compilar el proyecto, ver on_build_clicked) para que la etiqueta de
   estado y el panel de resultados se repinten mientras la operacion
   sigue corriendo, en vez de quedar congelados hasta que termine todo. */
static void pump_gtk_events(void) {
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void clear_table_list(AppState *state) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(state->table_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(state->table_listbox), child);
    }
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

/* generate_one_checked - si 'check' esta marcado, genera esa tabla
   (dbfiller_generate_table, ../generate.h -- la misma funcion que usa
   el CLI) y agrega el resultado al panel de resultados. */
static void generate_one_checked(GtkCheckButton *check, gpointer user_data) {
    AppState *state = user_data;
    if (!gtk_check_button_get_active(check)) return;

    const char *table_name = gtk_check_button_get_label(check);
    const char *repo_root = gtk_editable_get_text(GTK_EDITABLE(state->repo_root_entry));
    int force = gtk_check_button_get_active(GTK_CHECK_BUTTON(state->force_check));

    GenerateResult res;
    dbfiller_generate_table(state->conn, table_name, *repo_root ? repo_root : ".", force, &res);

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
 * del repo y va agregando cada linea de salida al panel de resultados a
 * medida que llega, en vez de volcarla toda junta al final. El proyecto
 * se compila dentro de un contenedor propio (ver Dockerfile: build de
 * libpq/liburing estaticos en un stage Alpine aparte) - un "make" nativo
 * en el host no tiene por que tener esas dependencias instaladas, asi
 * que la GUI usa el mismo camino de build que ya usa todo el resto del
 * proyecto (docker-compose.yml) en vez de asumir un toolchain nativo.
 * Sincronico (ver el comentario al principio del archivo):
 * pump_gtk_events() entre linea y linea es lo que evita que la ventana
 * quede completamente congelada mientras el build corre.
 */
static void on_build_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    const char *repo_root = gtk_editable_get_text(GTK_EDITABLE(state->repo_root_entry));
    if (!*repo_root) repo_root = ".";

    append_log(state, "--- docker compose build backend ---");
    set_status(state, "Compilando (Docker)...");
    pump_gtk_events();

    char *quoted_root = g_shell_quote(repo_root);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "cd %s && docker compose build backend 2>&1", quoted_root);
    g_free(quoted_root);

    FILE *proc = popen(cmd, "r");
    if (!proc) {
        append_log(state, "No se pudo lanzar 'docker compose build' (popen fallo).");
        set_status(state, "Error al compilar");
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), proc)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        append_log(state, line);
        pump_gtk_events();
    }

    int rc = pclose(proc);
    if (rc == 0) {
        append_log(state, "--- docker compose build: OK ---");
        set_status(state, "Compilacion exitosa");
    } else {
        append_log(state, "--- docker compose build: fallo (ver resultados arriba) ---");
        set_status(state, "Error al compilar");
    }
}

/*
 * on_copy_logs_clicked - copia el contenido completo del panel de
 * resultados al portapapeles del sistema (util para pegarlo en un
 * issue/chat sin tener que abrir una terminal a revisar el log).
 */
static void on_copy_logs_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppState *state = user_data;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(state->log_buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(state->log_buffer, &start, &end, FALSE);

    GdkClipboard *clipboard = gtk_widget_get_clipboard(state->log_view);
    gdk_clipboard_set_text(clipboard, text);
    g_free(text);

    append_log(state, "(logs copiados al portapapeles)");
}

static void on_app_shutdown(GtkApplication *app, gpointer user_data) {
    (void)app;
    AppState *state = user_data;
    if (state->conn) PQfinish(state->conn);
    g_free(state);
}

/* row_box - fila horizontal generica de formulario: label + widget +
   (opcional) boton, usada para las filas de "DATABASE_URL" y "Raiz del
   repo" para no repetir el mismo armado de GtkBox tres veces. */
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

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AppState *state = g_new0(AppState, 1);
    state->app = app;

    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "dbfiller -- generador de endpoints CRUD");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 640, 640);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(state->window), root);

    /* Conexion */
    state->db_url_entry = gtk_entry_new();
    const char *env_url = getenv("DATABASE_URL");
    if (env_url) gtk_editable_set_text(GTK_EDITABLE(state->db_url_entry), env_url);
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->db_url_entry), "postgresql://usuario:pass@host:5432/base");
    GtkWidget *connect_btn = gtk_button_new_with_label("Conectar");
    gtk_box_append(GTK_BOX(root), row_box("DATABASE_URL:", state->db_url_entry, connect_btn));

    state->status_label = gtk_label_new("Estado: sin conectar");
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0);
    gtk_box_append(GTK_BOX(root), state->status_label);

    /* Raiz del repo */
    state->repo_root_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(state->repo_root_entry), ".");
    GtkWidget *folder_btn = gtk_button_new_with_label("Elegir carpeta...");
    gtk_box_append(GTK_BOX(root), row_box("Raiz del repo:", state->repo_root_entry, folder_btn));

    /* Opciones */
    state->force_check = gtk_check_button_new_with_label("Sobreescribir archivos existentes (--force)");
    gtk_box_append(GTK_BOX(root), state->force_check);

    /* Tablas */
    GtkWidget *tables_label = gtk_label_new("Tablas ('public'):");
    gtk_label_set_xalign(GTK_LABEL(tables_label), 0.0);
    gtk_box_append(GTK_BOX(root), tables_label);

    GtkWidget *select_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *select_all_btn = gtk_button_new_with_label("Seleccionar todas");
    GtkWidget *select_none_btn = gtk_button_new_with_label("Ninguna");
    gtk_box_append(GTK_BOX(select_row), select_all_btn);
    gtk_box_append(GTK_BOX(select_row), select_none_btn);
    gtk_box_append(GTK_BOX(root), select_row);

    state->table_listbox = gtk_list_box_new();
    GtkWidget *tables_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tables_scroll), state->table_listbox);
    gtk_widget_set_size_request(tables_scroll, -1, 160);
    gtk_box_append(GTK_BOX(root), tables_scroll);

    GtkWidget *actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *generate_btn = gtk_button_new_with_label("Generar seleccionadas");
    gtk_widget_add_css_class(generate_btn, "suggested-action");
    GtkWidget *build_btn = gtk_button_new_with_label("Compilar proyecto (Docker)");
    gtk_widget_set_hexpand(generate_btn, TRUE);
    gtk_widget_set_hexpand(build_btn, TRUE);
    gtk_box_append(GTK_BOX(actions_row), generate_btn);
    gtk_box_append(GTK_BOX(actions_row), build_btn);
    gtk_box_append(GTK_BOX(root), actions_row);

    /* Resultados */
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
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_choose_folder_clicked), state);
    g_signal_connect(select_all_btn, "clicked", G_CALLBACK(on_select_all_clicked), state);
    g_signal_connect(select_none_btn, "clicked", G_CALLBACK(on_select_none_clicked), state);
    g_signal_connect(generate_btn, "clicked", G_CALLBACK(on_generate_clicked), state);
    g_signal_connect(build_btn, "clicked", G_CALLBACK(on_build_clicked), state);
    g_signal_connect(copy_logs_btn, "clicked", G_CALLBACK(on_copy_logs_clicked), state);
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
