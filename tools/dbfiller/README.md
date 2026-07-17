# dbfiller — generador de endpoints CRUD para cerver

CLI que se conecta a la base PostgreSQL del backend (`cerver`, la raíz de este
repo), lee la estructura real de una tabla (columnas, tipos, NOT NULL,
UNIQUE, PRIMARY KEY) y genera el código C de un endpoint CRUD completo para
esa tabla — siguiendo los mismos patrones que ya usan `controllers/sakila.c`,
`models/users.c` y `routes/index.h` (prepared statements asíncronos vía
`config/db.h`, `row_to_json`/`json_agg` armado en SQL, rutas `get()`/
`post_auth()`/etc.) — en vez de escribirlo a mano cada vez que aparece una
tabla nueva.

Solo Postgres, solo Linux, solo CLI. (Este archivo tuvo versiones anteriores
para SQLite/MySQL/MariaDB con GUI y cross-compile a Windows más una función de
llenar tablas con datos falsos — todo eso se descartó a favor de este objetivo
más chico y concreto: generar endpoints, no datos.)

## Qué genera

Por cada tabla `<tabla>`:

- `controllers/<tabla>.c/.h` — handlers HTTP: `list_<tabla>`, `get_<tabla>`,
  `create_<tabla>`, `update_<tabla>` (si la tabla tiene alguna columna
  actualizable), `delete_<tabla>`.
- `models/<tabla>.c/.h` — prepared statements y las funciones
  `<Tabla>_*_async` que los disparan (mismo patrón que `models/sakila.c`).
- Una entrada en `routes/index.h`:
  - `GET /api/<tabla>` y `GET /api/<tabla>/:id` — públicas.
  - `POST /api/<tabla>`, `PUT /api/<tabla>/:id`, `DELETE /api/<tabla>/:id` —
    exigen JWT válido (`Authorization: Bearer <token>`), igual que `/api/me`.
- Una entrada en `models/registry.h` (`<tabla>_register()`).

Todo archivo generado empieza con un comentario marcador; si ya existe un
archivo con ese nombre y **no** tiene el marcador (es decir, lo escribiste vos
a mano), dbfiller se niega a pisarlo salvo que pases `--force`. Correr
dbfiller de nuevo sobre la misma tabla actualiza su bloque en `routes/index.h`
y `models/registry.h` en vez de duplicarlo.

### Limitaciones conocidas (a propósito, no bugs)

- Solo tablas con **una** columna PRIMARY KEY. Tablas sin PK o con PK
  compuesta se saltean con un aviso.
- No se generan datos de FK ni se valida su existencia en C: una columna FK
  viaja como cualquier otra (parámetro de texto); si viola la constraint,
  Postgres devuelve error y el endpoint lo responde como `409`.
- `GET /api/<tabla>` trae un máximo fijo de 100 filas, sin paginación.
- Las columnas expuestas en el JSON de list/get/create/update quedan en una
  única línea fácil de editar (`#define <TABLA>_COLUMNS "..."` al principio
  de `models/<tabla>.c`) — **revisala antes de compilar** si la tabla tiene
  alguna columna que no debería exponerse (contraseñas, tokens, etc.).
- Los valores de texto del body JSON se copian tal cual (sin des-escapar
  `\"`/`\\`/`\uXXXX`), mismo límite ya aceptado en `controllers/auth.c` — no
  es una brecha de seguridad porque el valor siempre viaja parametrizado,
  nunca concatenado a SQL.

## Uso

Correr desde la raíz del repo (o pasar `--repo-root`), con `DATABASE_URL` ya
definida en el entorno (la misma variable que usa el backend, ver `.env`):

```bash
export DATABASE_URL=postgresql://usuario:pass@localhost:5432/mi_base

# Una tabla
tools/dbfiller/build/dbfiller --table productos

# Todas las tablas del schema 'public'
tools/dbfiller/build/dbfiller --all

# Sobreescribir archivos generados en una corrida anterior
tools/dbfiller/build/dbfiller --table productos --force
```

Después de correrlo, revisar el diff (`git diff`) antes de compilar: son
archivos nuevos en `controllers/`/`models/` más un par de líneas insertadas en
`routes/index.h`/`models/registry.h`.

## Requisito de setup (una sola vez)

`routes/index.h` y `models/registry.h` necesitan los comentarios-marcador
`/* dbfiller:includes-point */` y `/* dbfiller:routes-point */` (o
`/* dbfiller:models-point */`) para que dbfiller sepa dónde insertar cada
tabla nueva. Ya están agregados en este repo; si algún día se pierden (por
ejemplo, alguien reescribe esos archivos a mano), dbfiller falla con un error
explícito indicando cuál falta.

## Compilar

Requisitos (Linux): `gcc` y las cabeceras de desarrollo de PostgreSQL
(`libpq-dev` en Debian/Ubuntu, o el paquete equivalente de tu distro).

```bash
cd tools/dbfiller
make            # build/dbfiller
make clean
```

## Licencia

GPLv3 — ver [LICENSE](LICENSE).
