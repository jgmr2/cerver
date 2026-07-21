#include "dbfiller_jobs.h"
#include "dbfiller_json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "../utils/http/router.h"

typedef struct {
    int id;
    int used;
    pthread_mutex_t mutex;
    char *buf;
    size_t len;
    size_t cap;
    int done;
    int exit_code;
} Job;

static Job g_jobs[MAX_JOBS];
static pthread_mutex_t g_jobs_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_next_job_id = 1;
static int g_jobs_init_done = 0;

static void jobs_init_once(void) {
    /* Los mutex de cada slot se inicializan una sola vez, la primera vez
       que job_start() los necesita -- un inicializador estatico por
       slot (como PTHREAD_MUTEX_INITIALIZER) no alcanza porque son
       MAX_JOBS instancias en un arreglo, no una sola global. */
    if (g_jobs_init_done) return;
    for (int i = 0; i < MAX_JOBS; i++) pthread_mutex_init(&g_jobs[i].mutex, NULL);
    g_jobs_init_done = 1;
}

static void job_append_line(Job *job, const char *line, size_t line_len) {
    pthread_mutex_lock(&job->mutex);
    size_t need = job->len + line_len + 2; /* +1 '\n' +1 NUL */
    if (need > job->cap) {
        size_t new_cap = job->cap ? job->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        char *grown = realloc(job->buf, new_cap);
        if (grown) { job->buf = grown; job->cap = new_cap; }
    }
    if (job->buf && job->len + line_len + 1 < job->cap) {
        memcpy(job->buf + job->len, line, line_len);
        job->len += line_len;
        job->buf[job->len++] = '\n';
        job->buf[job->len] = '\0';
    }
    pthread_mutex_unlock(&job->mutex);
}

typedef struct {
    Job *job;
    char *cmd;
} JobThreadArg;

/* job_thread_main - hilo de fondo de un job: mismo patron que
   run_streaming_command de la vieja GUI GTK4 (popen + fgets linea por
   linea), pero appendeando al buffer del Job (con su mutex) en vez de a
   un GtkTextBuffer, y sin pump_gtk_events (no hay loop de UI que
   mantener responsive del lado del servidor -- eso es trabajo del
   cliente, que hace polling). */
static void *job_thread_main(void *arg) {
    JobThreadArg *ta = arg;
    Job *job = ta->job;
    char *cmd = ta->cmd;
    free(ta);

    FILE *proc = popen(cmd, "r");
    int exit_code = -1;
    if (proc) {
        char line[4096];
        while (fgets(line, sizeof(line), proc)) {
            size_t len = strlen(line);
            if (len && line[len - 1] == '\n') line[--len] = '\0';
            job_append_line(job, line, len);
        }
        int status = pclose(proc);
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    free(cmd);

    pthread_mutex_lock(&job->mutex);
    job->done = 1;
    job->exit_code = exit_code;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

int job_start(const char *cmd) {
    jobs_init_once();

    pthread_mutex_lock(&g_jobs_table_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!g_jobs[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_jobs_table_mutex);
        return -1;
    }

    Job *job = &g_jobs[slot];
    job->used = 1;
    job->id = g_next_job_id++;
    job->len = 0;
    job->done = 0;
    job->exit_code = 0;
    if (!job->buf) {
        job->cap = 4096;
        job->buf = malloc(job->cap);
    }
    if (job->buf) job->buf[0] = '\0';
    int id = job->id;
    pthread_mutex_unlock(&g_jobs_table_mutex);

    JobThreadArg *ta = malloc(sizeof(JobThreadArg));
    ta->job = job;
    ta->cmd = strdup(cmd);

    pthread_t t;
    if (pthread_create(&t, NULL, job_thread_main, ta) != 0) {
        free(ta->cmd);
        free(ta);
        pthread_mutex_lock(&job->mutex);
        job->done = 1;
        job->exit_code = -1;
        pthread_mutex_unlock(&job->mutex);
        return id;
    }
    pthread_detach(t); /* nadie hace join: el resultado se consulta via job_poll */

    return id;
}

int job_poll(int id, char **out_log, int *out_done, int *out_exit_code) {
    pthread_mutex_lock(&g_jobs_table_mutex);
    Job *job = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].used && g_jobs[i].id == id) { job = &g_jobs[i]; break; }
    }
    pthread_mutex_unlock(&g_jobs_table_mutex);
    if (!job) return -1;

    pthread_mutex_lock(&job->mutex);
    *out_log = strdup(job->buf ? job->buf : "");
    *out_done = job->done;
    *out_exit_code = job->exit_code;
    pthread_mutex_unlock(&job->mutex);
    return 0;
}

void job_poll_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    const char *id_str = route_param("id");
    int id = id_str ? atoi(id_str) : -1;

    char *log = NULL;
    int done = 0, exit_code = 0;
    if (id <= 0 || job_poll(id, &log, &done, &exit_code) != 0) {
        send_res(r, f, "404 Not Found", "application/json", "{\"error\":\"job no encontrado\"}");
        return;
    }

    size_t escaped_sz = strlen(log) * 2 + 64;
    char *escaped = malloc(escaped_sz);
    dbfiller_json_escape(escaped, escaped_sz, log);
    free(log);

    size_t json_sz = escaped_sz + 128;
    char *json = malloc(json_sz);
    snprintf(json, json_sz, "{\"done\":%s,\"exit_code\":%d,\"log\":\"%s\"}",
             done ? "true" : "false", exit_code, escaped);
    free(escaped);

    send_json(r, f, json);
    free(json);
}
