# DB Init con Sakila

Este proyecto ahora usa Sakila como dataset de pruebas. El contenedor de PostgreSQL carga automaticamente los scripts dentro de `/docker-entrypoint-initdb.d` solo cuando el volumen `pgdata` es nuevo.

Orden de ejecucion:
1. `01_auth_schema.sql` (tabla `users`, ver utils/auth/ y controllers/auth.c)
2. `10_load_sakila.sql`
3. `20_indexes.sql`

**Importante:** estos scripts solo corren automaticamente cuando el volumen
`pgdata` es nuevo (primera vez que se crea el contenedor). Si agregas un
script nuevo a esta carpeta y ya tenias el volumen de una sesion anterior,
Postgres NO lo va a correr solo — hay que aplicarlo a mano:

```bash
docker compose exec -T db psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -f - < db/init/NN_nuevo.sql
```

o, si preferis partir de cero (se pierden los datos, incluidos usuarios
registrados):

```bash
docker compose down -v
docker compose up -d
```

`10_load_sakila.sql` incluye:
- `/sakila/10_sakila_schema.sql`
- `/sakila/11_sakila_data.sql`

`20_indexes.sql` crea indices para mejorar consultas hot como:
- `WHERE length IS NOT NULL ORDER BY length DESC, title ASC LIMIT 10`

## Reinicializar DB y recargar Sakila

```bash
docker compose down -v
docker compose up -d db
```

## Validar tablas de Sakila

```bash
docker compose exec db psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "SELECT count(*) AS films FROM film;"
docker compose exec db psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -c "SELECT count(*) AS actors FROM actor;"
```

## Levantar stack completo

```bash
docker compose up -d
```
