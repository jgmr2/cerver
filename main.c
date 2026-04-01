#include <stdlib.h>
#include <uv.h>
#include "config/db.h"
#include "routes/index.h"

void alloc(uv_handle_t* h, size_t s, uv_buf_t* b) { *b = uv_buf_init(malloc(s + 1), s); }

void on_rd(uv_stream_t* c, ssize_t n, const uv_buf_t* b) {
    n > 0 ? (b->base[n] = 0, router(c, b->base)) : (n < 0 && !uv_is_closing((uv_handle_t*)c)) ? uv_close((uv_handle_t*)c, (uv_close_cb)free) : (void)0;
    free(b->base);
}

void on_new(uv_stream_t* s, int st) {
    uv_tcp_t* c = st < 0 ? NULL : malloc(sizeof(*c));
    if (c) uv_tcp_init(s->loop, c), uv_accept(s, (uv_stream_t*)c) ? uv_close((uv_handle_t*)c, (uv_close_cb)free) : uv_read_start((uv_stream_t*)c, alloc, on_rd);
}

int main() {
    uv_loop_t *loop = uv_default_loop();
    
    // Le pasamos el loop a la base de datos para los sockets no bloqueantes
    init_db(loop); 
    
    uv_tcp_t s; 
    struct sockaddr_in a;
    uv_tcp_init(loop, &s);
    uv_ip4_addr("0.0.0.0", 8080, &a);
    uv_tcp_bind(&s, (struct sockaddr*)&a, 0);
    
    if (uv_listen((uv_stream_t*)&s, 1024, on_new)) return 1;
    
    return uv_run(loop, UV_RUN_DEFAULT);
}