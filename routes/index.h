#ifndef ROUTES_INDEX_H
#define ROUTES_INDEX_H

#include "../utils/http/router.h"
// Importamos los controladores que vayamos creando
#include "../controllers/sakila.h"
#include "../controllers/home.h"

static inline void init_routes() {
    get("/api", api);
    get("/api/sakila/films/top", get_sakila_top_films);
    get("/api/sakila/actors/top", get_sakila_top_actors);
}

#endif