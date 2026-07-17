# DB Init

El contenedor de PostgreSQL carga automáticamente los scripts `.sql` de esta
carpeta (`/docker-entrypoint-initdb.d`), en orden alfabético, la primera vez
que se crea el volumen `pgdata`.

Hoy solo hay uno:

- `01_auth_schema.sql` — tabla `users` (autenticación propia JWT, ver
  `utils/auth/` y `utils/auth/auth.c`). Es infraestructura del boilerplate,
  no del esquema de negocio de un proyecto en particular.

## Agregar el esquema de un proyecto nuevo

1. Agregá tus propias tablas en un script nuevo, por ejemplo
   `02_mi_esquema.sql` (el prefijo numérico define el orden de carga).
2. Levantá la base (`docker compose up -d db`, o `docker compose down -v &&
   docker compose up -d db` si el volumen `pgdata` ya existía y no vas a
   perder nada importante).
3. Corré [`tools/dbfiller`](../../tools/dbfiller/README.md) apuntando a esa
   base para generar los endpoints CRUD de cada tabla — es la forma
   pensada de llenar `controllers/`/`models/`/`routes/index.h`, en vez de
   escribirlos a mano.

**Importante:** estos scripts solo corren automáticamente cuando el volumen
`pgdata` es nuevo. Si agregás un script a esta carpeta y ya tenías el volumen
de una sesión anterior, Postgres NO lo va a correr solo — hay que aplicarlo a
mano:

```bash
docker compose exec -T db psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -f - < db/init/NN_nuevo.sql
```

o partir de cero (se pierden los datos, incluidos usuarios registrados):

```bash
docker compose down -v
docker compose up -d
```
