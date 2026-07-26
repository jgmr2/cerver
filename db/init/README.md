# DB Init

El contenedor de PostgreSQL carga automáticamente los scripts `.sql` de esta
carpeta (`/docker-entrypoint-initdb.d`), en orden alfabético, la primera vez
que se crea el volumen `pgdata`.

Hoy solo hay uno:

- `02_inventory.sql` — esquema de negocio de este proyecto (inventario de
  ítems, reportes, solicitudes) que además incluye su propia tabla de
  autenticación (`"user"`, con `email`/`password_hash`). No hay ningún
  script de auth aparte: `tools/dbfiller` detecta automáticamente cuál
  tabla del esquema sirve para login (ver
  [`tools/dbfiller/README.md`](../../tools/dbfiller/README.md#autenticación))
  y genera `utils/auth/auth_model.c/h`/`utils/auth/auth.c` a partir de esa
  tabla — no hace falta un `users` hardcodeado en el boilerplate.

## Agregar el esquema de un proyecto nuevo

1. Agregá tus propias tablas en un script nuevo, por ejemplo
   `02_mi_esquema.sql` (el prefijo numérico define el orden de carga). Si
   tu esquema no trae una tabla que sirva para login (PK simple + columna
   `email`/`username` única y `NOT NULL` + una columna tipo
   `password_hash`), `tools/dbfiller` no genera auth y `utils/auth/*`
   queda como esté (o hay que agregar esa tabla).
2. Levantá la base (`docker compose up -d db`, o `docker compose down -v &&
   docker compose up -d db` si el volumen `pgdata` ya existía y no vas a
   perder nada importante).
3. Corré [`tools/dbfiller`](../../tools/dbfiller/README.md)
   (`python3 tools/dbfiller/generate.py`) para generar los endpoints CRUD
   de cada tabla a partir de ese mismo `.sql` — es la forma pensada de
   llenar `controllers/`/`models/`/`routes/index.h` (y, si corresponde,
   `utils/auth/`), en vez de escribirlos a mano. No necesita la base
   levantada: lee el `.sql` directo.

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
