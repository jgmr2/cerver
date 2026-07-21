#include "scaffold.h"
#include "boilerplate_zip.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* dir_is_empty - 1 si path no existe todavia, o existe y no tiene entradas
   aparte de "."/"..". 0 si tiene contenido o no se pudo abrir por otra
   razon (permisos, etc - el llamador lo trata como "no esta vacio" para
   no arriesgarse). */
static int dir_is_empty_or_missing(const char *path) {
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT;

    struct dirent *entry;
    int empty = 1;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        empty = 0;
        break;
    }
    closedir(d);
    return empty;
}

/* mkdir_recursive - equivalente a `mkdir -p`: crea cada componente del
   path que falte. EEXIST en cualquier paso se ignora (el componente ya
   estaba, es el caso comun para dest_dir cuando el llamador ya valido con
   dir_is_empty_or_missing que esta vacio o no existe). */
static int mkdir_recursive(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* run_unzip - fork+execvp de "unzip -q -o <zip_path> -d <dest_dir>". Sin
   shell de por medio (execvp toma cada argumento literal): dest_dir viene
   de un dialogo de carpeta o de argv, puede traer espacios u otros
   caracteres que armar un string de shell obligaria a escapar. Retorna 0
   si unzip salio con status 0, -1 en cualquier otro caso (err queda
   lleno). */
static int run_unzip(const char *zip_path, const char *dest_dir, char *err, size_t err_len) {
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_len, "fork() fallo: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char *argv[] = {(char *)"unzip", (char *)"-q", (char *)"-o", (char *)zip_path, (char *)"-d", (char *)dest_dir, NULL};
        execvp("unzip", argv);
        _exit(127); /* unzip no esta instalado / no esta en PATH */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            snprintf(err, err_len, "no se encontro el binario 'unzip' en PATH (instalar el paquete 'unzip')");
        } else {
            snprintf(err, err_len, "unzip termino con error (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
        return -1;
    }
    return 0;
}

int scaffold_new_project(const char *dest_dir, char *err, size_t err_len) {
    if (!dir_is_empty_or_missing(dest_dir)) {
        snprintf(err, err_len, "'%s' ya existe y no esta vacio - elegi una carpeta nueva o vacia", dest_dir);
        return -1;
    }
    if (mkdir_recursive(dest_dir) != 0) {
        snprintf(err, err_len, "no se pudo crear '%s': %s", dest_dir, strerror(errno));
        return -1;
    }

    char zip_path[512];
    snprintf(zip_path, sizeof(zip_path), "/tmp/dbfiller_boilerplate_%d.zip", (int)getpid());

    FILE *f = fopen(zip_path, "wb");
    if (!f) {
        snprintf(err, err_len, "no se pudo crear '%s': %s", zip_path, strerror(errno));
        return -1;
    }
    size_t written = fwrite(DBFILLER_BOILERPLATE_ZIP, 1, DBFILLER_BOILERPLATE_ZIP_LEN, f);
    fclose(f);
    if (written != DBFILLER_BOILERPLATE_ZIP_LEN) {
        snprintf(err, err_len, "escritura incompleta del zip temporal en '%s'", zip_path);
        remove(zip_path);
        return -1;
    }

    int rc = run_unzip(zip_path, dest_dir, err, err_len);
    remove(zip_path);
    return rc;
}
