# dbfiller — generador de endpoints CRUD para cerver

Script Python que lee el esquema ya commiteado en `db/init/*.sql` (los
mismos `.sql` que carga Postgres al arrancar, ver `db/init/README.md`) y
genera el código C de un endpoint CRUD completo por cada `CREATE TABLE` —
siguiendo los mismos patrones que ya usa `utils/auth/users.c` (prepared
statements asíncronos vía `config/db.h`, `row_to_json`/`json_agg` armado en
SQL, rutas `get()`/`post_auth()`/etc.) — en vez de escribirlo a mano cada
vez que aparece una tabla nueva.

No necesita una base Postgres corriendo ni ninguna conexión de red: todo
sale de parsear el texto del `.sql`. Es un paso **manual**, no una etapa
de `docker build` — se corre a mano, se revisa el diff, y el resultado se
commitea como cualquier otro cambio de código (ver "Uso" abajo). Así el
código generado se puede editar después a mano para agregarle lógica de
negocio sin que un build futuro lo pise: el marcador de la primera línea
de cada archivo generado ya protege esas ediciones de una regeneración
sin `--force`.

(Este archivo tuvo versiones anteriores bastante más grandes: un CLI en C
que introspeccionaba una base Postgres real vía libpq, una GUI GTK4, y
después un backend HTTP + frontend Svelte que hacían lo mismo por web,
con Docker-outside-of-Docker para manejar contenedores. Todo eso se
descartó a propósito a favor de esto: el esquema ya es texto en el repo,
no hace falta una base viva para leerlo.)

## Qué genera

Por cada tabla `<tabla>` de `db/init/*.sql`, todo bajo `src/` (el código
que se replica en cada proyecto nuevo, ver `README.md` de la raíz):

- `src/controllers/<tabla>.c/.h` — handlers HTTP: `list_<tabla>`,
  `get_<tabla>`, `create_<tabla>`, `update_<tabla>` (si la tabla tiene
  alguna columna actualizable), `delete_<tabla>`.
- `src/models/<tabla>.c/.h` — prepared statements y las funciones
  `<Tabla>_*_async` que los disparan (mismo patrón que
  `src/utils/auth/users.c`).
- Una entrada en `src/routes/index.h`:
  - `GET /api/<tabla>` y `GET /api/<tabla>/:id` — públicas.
  - `POST /api/<tabla>`, `PUT /api/<tabla>/:id`, `DELETE /api/<tabla>/:id` —
    exigen JWT válido (`Authorization: Bearer <token>`), igual que `/api/me`.
- Una entrada en `src/models/registry.h` (`<tabla>_register()`).
- Si `docs-ui/openapi.yaml` existe en el repo destino, sus paths
  (`/api/<tabla>`, `/api/<tabla>/{id}`) y schemas (`<Tabla>`,
  `<Tabla>Create`, `<Tabla>Update`) — queda documentado y probable desde
  Swagger UI (`/docs`) sin tocar nada a mano. Opcional: si el archivo no
  existe, este paso simplemente se saltea (no hace falta Swagger para que
  el resto funcione).

Todo archivo generado empieza con un comentario marcador **y una segunda
línea con un hash del contenido que le sigue**. Eso es lo que le permite a
dbfiller distinguir tres situaciones al volver a correrlo:

- El archivo no existe → lo genera.
- Existe pero no tiene ese marcador+hash (lo escribiste vos a mano desde
  el principio) → se niega a pisarlo salvo `--force`.
- Existe, tiene marcador+hash, **pero el hash ya no coincide** con el
  contenido actual (lo generaste, después le agregaste lógica de negocio
  a mano, y ahora estás regenerando porque cambió el esquema) → también
  se niega, mismo mensaje, salvo `--force`. Sin el hash esto no se podría
  detectar: el marcador solo dice "esto lo generó dbfiller alguna vez",
  no "esto sigue igual a como lo generó la última vez".
- Existe, tiene marcador+hash, y el hash coincide (nadie lo tocó desde la
  última generación) → lo sobreescribe sin pedir nada.

O sea: podés editar un archivo generado para agregarle lógica de negocio
con total libertad; la única vez que hace falta `--force` es cuando
correr dbfiller de nuevo pisaría justo esos cambios (porque cambiaste el
esquema, no porque cambiaste el archivo generado sin querer) — ahí
conviene revisar el diff antes de confirmar. Correr dbfiller de nuevo
actualiza su bloque en `src/routes/index.h` y `src/models/registry.h` en
vez de duplicarlo (esos parches por bloque no llevan hash: son una línea
fija por tabla, pensada para no editarse a mano).

## Autenticación

`utils/auth/` no tiene ningún nombre de tabla hardcodeado: dbfiller
detecta, entre las tablas del esquema, cuál sirve para login y genera
`src/utils/auth/auth_model.c/.h` + `src/utils/auth/auth.c` a partir de
ella (mismo mecanismo de marcador/`--force` que el resto). No hay CRUD
genérico para esa tabla (nada de `list_.../update_.../delete_...`) —
solo `POST /api/auth/register`, `POST /api/auth/login` y `GET /api/me`
(ver `src/utils/auth/auth.h`, que no cambia).

Una tabla califica como tabla de autenticación si tiene:

- PK de una sola columna (igual que cualquier tabla que dbfiller procese).
- una columna llamada exactamente `email` o `username` (sin importar
  mayúsculas) que sea `UNIQUE` y `NOT NULL` — el identificador de login.
- exactamente una columna que matchee el mismo patrón de "columna
  sensible" que ya excluye del CRUD genérico (`password`, `hash`, etc.)
  — la columna de password.

Si **ninguna** tabla califica, `utils/auth/` queda como esté (no es un
error). Si **más de una** califica, dbfiller para con un error listando
los candidatos — nunca elige "la primera" en silencio, hay que
desambiguar con `--auth-table <tabla>` o sacar/renombrar una de las
tablas del `--schema` usado. `--auth-table none` desactiva la detección
por completo.

El body de `POST /api/auth/register` siempre usa las claves fijas
`"username"`/`"password"` (no dependen del nombre real de la columna —
`"username"` puede representar un email), más cualquier otra columna
`NOT NULL` sin default que tenga la tabla (ej. `first_name`, `fk_role`):
esas viajan tal cual, igual que en un `create_<tabla>` genérico, y si
faltan dan 400. El JWT resultante sigue con los claims `"sub"`/
`"username"` de siempre (ver `utils/auth/jwt.h`, sin cambios).

### Limitaciones conocidas (a propósito, no bugs)

- Solo tablas con **una** columna PRIMARY KEY, declarada inline, como
  constraint de tabla dentro del `CREATE TABLE`, o agregada después con
  `ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY (...)` (el formato que
  emite `pg_dump`). Tablas sin PK o con PK compuesta se saltean con un
  aviso, sin que el proceso termine en error (exit code 0) — es un caso
  esperado, a diferencia de un error real (archivo escrito a mano sin
  `--force`, marcador faltante).
- Subconjunto de DDL estándar de Postgres, no cualquier SQL válido: tipos
  comunes (`INTEGER`, `TEXT`, `NUMERIC(p,s)`, `VARCHAR(n)`, `TIMESTAMPTZ`,
  `DOUBLE PRECISION`, `CHARACTER VARYING(n)`, `TIMESTAMP [(n)] WITH/WITHOUT
  TIME ZONE`, `SERIAL`/`BIGSERIAL`, `GENERATED ... AS IDENTITY`, etc.), sin
  parser SQL completo por detrás más que `sqlparse` para separar
  statements.
- No se generan datos de FK ni se valida su existencia en C: una columna FK
  viaja como cualquier otra (parámetro de texto); si viola la constraint,
  Postgres devuelve error y el endpoint lo responde como `409`.
- `GET /api/<tabla>` trae un máximo fijo de 100 filas, sin paginación.
- Las columnas expuestas en el JSON de list/get/create/update quedan en una
  única línea fácil de editar (`#define <TABLA>_COLUMNS "..."` al principio
  de `models/<tabla>.c`). Columnas cuyo nombre contenga `password`, `passwd`,
  `hash`, `secret`, `token`, `api_key`/`apikey` o `credential` (sin importar
  mayúsculas, sea cual sea la tabla — no hay ningún nombre de tabla
  hardcodeado, solo un patrón sobre el nombre de columna) se excluyen
  automáticamente de list/get/create/update, con un comentario en el archivo
  generado indicando cuáles y cómo reincluirlas a mano si hiciera falta. Es
  una heurística, no una garantía — **revisá igual antes de compilar** por si
  la tabla tiene alguna otra columna sensible con un nombre que el filtro no
  detecte.
- Los valores de texto del body JSON se copian tal cual (sin des-escapar
  `\"`/`\\`/`\uXXXX`), mismo límite ya aceptado en `utils/auth/auth.c` — no
  es una brecha de seguridad porque el valor siempre viaja parametrizado,
  nunca concatenado a SQL.

## Uso

Paso manual, parado en la raíz del repo — agregar una tabla nueva es:
escribir su `CREATE TABLE` en un `db/init/NN_*.sql` nuevo (ver
`db/init/README.md`), correr dbfiller, revisar el diff, commitear.

### Con Python local

```bash
python3 -m venv .venv && source .venv/bin/activate   # opcional
pip install -r tools/dbfiller/requirements.txt

# Todas las tablas de db/init/*.sql (default), parado en la raíz del repo
python3 tools/dbfiller/generate.py

# Archivo(s) puntuales
python3 tools/dbfiller/generate.py --schema db/init/02_mi_esquema.sql

# Otra raíz de repo (proyecto forkeado de este mismo boilerplate)
python3 tools/dbfiller/generate.py --repo-root ../mi-otro-proyecto

# Sobreescribir archivos generados en una corrida anterior
python3 tools/dbfiller/generate.py --force
```

### Sin Python instalado (vía Docker, una línea)

```bash
docker run --rm -v "$(pwd)":/app -w /app python:3.12-alpine \
  sh -c "pip install -q sqlparse && python3 tools/dbfiller/generate.py"
```

Después de correrlo, revisar el diff (`git diff`) antes de compilar: son
archivos nuevos en `src/controllers/`/`src/models/` más un par de líneas
insertadas en `src/routes/index.h`/`src/models/registry.h`. `docker build
.`/`make -C src` después de esto compilan lo que quedó commiteado, sin
tocar el esquema para nada — no hace falta Python ni este paso para
compilar, solo para generar código nuevo.

## Proyecto nuevo

No hay `--scaffold`: este repo (`cerver`) **es** el boilerplate — `src/`
ya tiene `core/`, `config/`, `utils/`, autenticación JWT, y en la raíz
`Dockerfile`/`docker-compose.yml`. Un proyecto nuevo se arranca haciendo
fork o clonando este repo, agregando el esquema propio en `db/init/`, y
corriendo dbfiller a mano para generar sus endpoints.

## Requisito de setup (una sola vez)

`src/routes/index.h` y `src/models/registry.h` necesitan los
comentarios-marcador `/* dbfiller:includes-point */` y
`/* dbfiller:routes-point */` (o `/* dbfiller:models-point */`) para que
dbfiller sepa dónde insertar cada tabla nueva. Ya están agregados en este
repo; si algún día se pierden (por ejemplo, alguien reescribe esos
archivos a mano), dbfiller falla con un error explícito indicando cuál
falta.

Mismo mecanismo para `docs-ui/openapi.yaml` (opcional, ver arriba), con
marcadores estilo comentario YAML: `# dbfiller:schemas-point` (dentro de
`components.schemas`) y `# dbfiller:paths-point` (al final de `paths`).

## Licencia

GPLv3 — ver [LICENSE](LICENSE).
