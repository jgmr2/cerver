# dbfiller — generador de endpoints CRUD para cerver

CLI que se conecta a la base PostgreSQL del backend (`cerver`, la raíz de este
repo), lee la estructura real de una tabla (columnas, tipos, NOT NULL,
UNIQUE, PRIMARY KEY) y genera el código C de un endpoint CRUD completo para
esa tabla — siguiendo los mismos patrones que ya usa `utils/auth/users.c`
(prepared statements asíncronos vía `config/db.h`, `row_to_json`/`json_agg`
armado en SQL, rutas `get()`/`post_auth()`/etc.) — en vez de escribirlo a
mano cada vez que aparece una tabla nueva.

Solo Postgres, solo Linux. Hay dos front-ends, CLI y GUI (GTK4) — ambos llaman
a la misma función de orquestación (`dbfiller_generate_table`, `src/generate.h`),
así que se comportan idéntico y no hay dos implementaciones del mismo flujo
para mantener sincronizadas. (Este archivo tuvo versiones anteriores para
SQLite/MySQL/MariaDB con GUI en Nuklear y cross-compile a Windows más una
función de llenar tablas con datos falsos — todo eso se descartó a favor de
este objetivo más chico y concreto: generar endpoints, no datos.)

## Qué genera

Por cada tabla `<tabla>`:

- `controllers/<tabla>.c/.h` — handlers HTTP: `list_<tabla>`, `get_<tabla>`,
  `create_<tabla>`, `update_<tabla>` (si la tabla tiene alguna columna
  actualizable), `delete_<tabla>`.
- `models/<tabla>.c/.h` — prepared statements y las funciones
  `<Tabla>_*_async` que los disparan (mismo patrón que `utils/auth/users.c`).
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

# Crear un proyecto cerver nuevo desde cero (no requiere DATABASE_URL ni
# un checkout de cerver al lado, ver "Proyecto nuevo (scaffold)" abajo)
tools/dbfiller/build/dbfiller --scaffold ../mi-cliente-nuevo
```

Después de correrlo, revisar el diff (`git diff`) antes de compilar: son
archivos nuevos en `controllers/`/`models/` más un par de líneas insertadas en
`routes/index.h`/`models/registry.h`.

## Interfaz gráfica

`make gui` compila `build/dbfiller-gui` (GTK4), con tres pestañas: **Tablas**
(conectar, configurar el proyecto, cargar esquema y generar CRUD — ver
abajo), **Docker** y **Carga**.

### Tablas — wizard de 6 pasos

Todo lo que hace falta para levantar y trabajar contra un proyecto vive en
esta única pestaña, como un wizard: **Atrás/Siguiente** navegan entre pasos
(navegación pura, ningún paso se bloquea ni se valida — los botones de
acción de cada paso siguen siendo los que hacen el trabajo real), y
"Resultados" al final queda siempre visible sin importar en qué paso estés.
Antes "Esquema" era una pestaña aparte, separada de donde se conecta/
configura la base, lo cual era confuso (¿esquema de cuál base?) y escondía
un bug real (ver más abajo); y antes de ser un wizard, los 6 pasos de abajo
eran ~30 controles amontonados en una sola pantalla.

1. **Conexión** — `DATABASE_URL` a mano, o vía **"Listar bases del servidor"**
   (conecta con el host/usuario/contraseña ya escritos, dbname se ignora
   para este paso, y muestra las bases del servidor; elegir una arma la
   `DATABASE_URL` real y conecta). **"Postgres de prueba"** (Levantar/
   Detener) es una alternativa más: un Postgres desechable aparte para
   probar sin depender de una base real — ninguna de las dos opciones
   reemplaza la otra, `DATABASE_URL` sigue siendo un campo de texto normal.
2. **Proyecto** — Raíz del repo (con selector) — dónde viven `controllers/`,
   `models/`, `routes/`, `db/init/`, etc. **"Crear proyecto nuevo
   (boilerplate)..."** crea un proyecto cerver nuevo ahí mismo (la misma
   carpeta, sin abrir un segundo selector aparte — antes lo hacía, y
   terminaba pidiendo la misma carpeta dos veces por dos caminos distintos).
3. **Variables de entorno (.env)** — `POSTGRES_USER`/`PASSWORD`/`DB`/
   `JWT_SECRET`, con **"Generar valores por defecto"** (password y
   JWT_SECRET aleatorios de `/dev/urandom`, no un generador pseudoaleatorio
   de uso general) y **"Guardar .env"** (se niega a pisar un `.env`
   existente salvo que se tilde "Sobreescribir .env existente" — no está en
   git, no hay forma de recuperarlo si se pisa sin querer).
4. **Esquema** — **"Cargar .sql..."** copia el archivo numerado a
   `db/init/<NN>_<archivo>.sql` (mismo criterio que documenta
   `db/init/README.md`) y lo aplica de inmediato. **Importante:** se aplica
   contra la `DATABASE_URL` de arriba (test, externa, lo que sea) — **no**
   contra "el servicio `db` del `docker-compose.yml` de este repo", que es
   lo que hacía antes (bug real: si se había elegido otra base con "Listar
   bases"/"Postgres de prueba", el esquema podía terminar en el servidor
   equivocado, o fallar si ese `db` no estaba corriendo). Como el host no
   tiene `psql` instalado, se usa un contenedor `postgres:16-alpine`
   desechable (`docker run --rm -i --network=host ... psql "$DATABASE_URL"
   < archivo`) — `--network=host` para poder alcanzar `localhost:<puerto>`
   o cualquier host/IP externo igual que un proceso nativo del host. El
   archivo se manda por stdin, nunca vía `-f` apuntando a un path dentro de
   un contenedor (ese bind mount puede quedar desincronizado del host sin
   que nada lo note). Al terminar, muestra `\dt` en el mismo panel para
   confirmar de una qué tablas quedaron. La lista de abajo muestra todos los
   `db/init/*.sql` ya cargados (menos `01_auth_schema.sql`, infraestructura
   fija) con un botón **"Aplicar de nuevo"** por archivo — volver a elegir
   el mismo archivo desde "Cargar .sql..." en vez de usar este botón también
   funciona (se detecta que ya está en `db/init/` y se reaplica sin
   duplicarlo), pero "Aplicar de nuevo" es el camino directo.
5. **Tablas ('public')** — lista con checkboxes, **"Generar seleccionadas"**
   (llama a `dbfiller_generate_table()`, `src/generate.h`, la misma función
   que usa el CLI — un cambio en el generador se ve en los dos front-ends
   sin tocar nada más) y **"Compilar proyecto (Docker)"** (`docker compose
   build backend`, no `make` nativo: el proyecto se compila dentro de su
   propio contenedor — ver `Dockerfile` — y un host sin esas librerías de
   desarrollo instaladas no puede correr `make` directo).
6. **Probar endpoints** — último paso: **"Levantar (docker compose up -d)"**
   por si los servicios no están corriendo todavía, y una lista con cada
   endpoint ya registrado en `routes/index.h` del proyecto actual (mismo
   escaneo — `scan_routes_file` — que usa el desplegable de la pestaña
   Carga), marcados `(JWT)` si exigen `Authorization`. Un campo
   `Authorization` compartido y un botón **"Probar"** por fila disparan un
   solo request (`curl`, no Apache Bench — esto es un smoke test rápido,
   "¿responde y con qué código?", no una prueba de carga) y el resultado
   queda en "Resultados". Se repuebla sola al entrar a este paso.

Panel de resultados único para todo lo anterior, con **"Copiar logs"**.

### Docker

Un botón **"Refrescar"** corre `docker compose ps`, `docker stats --no-stream`
y `docker images` y muestra cada uno en su propio panel. El checkbox
**"Actualizar stats en vivo (cada 2s)"** prende un polling liviano (solo
`docker stats`, no los otros dos comandos) vía `g_timeout_add_seconds` — sigue
sin haber un hilo de fondo (el timer corre en el mismo loop principal de GTK,
entre eventos), así que no contradice la simplificación de abajo. Sin
controles de start/stop/restart de containers en esta versión.

### Carga

Pruebas de estrés con **Apache Bench** (`ab`, paquete `apache2-utils`). El
desplegable **"Endpoint"** (con su botón **"Refrescar"**) lista lo ya
registrado en `routes/index.h` del proyecto en "Raíz del repo" (pestaña
Tablas) — elegir uno precarga Path y Método, en vez de tener que saber de
memoria qué endpoints existen. El resto: método (GET/POST — `ab` no tiene
una forma portable de pedir un método arbitrario, así que PUT/DELETE quedan
fuera de esta versión), body JSON (solo POST), header `Authorization:
Bearer <token>` opcional (el token se consigue a mano, ej. `curl
.../api/auth/login`), cantidad de requests, concurrencia y keep-alive. El
resultado es el resumen que imprime `ab` (percentiles de latencia,
requests/s, fallos), con un botón **"Copiar logs"** para llevarlo al
portapapeles (mismo botón que ya tenía la pestaña Tablas).

Para un chequeo rápido de "¿este endpoint responde?" sin armar una prueba de
carga, ver el paso **"Probar endpoints"** del wizard de la pestaña Tablas —
mismo escaneo de `routes/index.h`, pero un solo request por botón.

### Postgres de prueba (pestaña Tablas)

Los botones **"Levantar"/"Detener"** junto al campo `DATABASE_URL` manejan un
Postgres desechable aparte (`postgres:16-alpine`, puerto `5433`, datos en
`tmpfs` — se reinicia vacío en cada `up`), pensado para probar la
introspección/generación de dbfiller sin necesitar una base real a mano.
"Levantar" precarga `DATABASE_URL` con la conexión de prueba; no toca ni
reemplaza el flujo de conectarse a una base externa (la de un cliente real,
la de este mismo repo, la que sea) — el campo sigue siendo editable a mano
como siempre. Ver `src/testdb.h`.

### Proyecto nuevo (scaffold)

El botón **"Crear proyecto nuevo (boilerplate)..."** (pestaña Tablas) — o
`dbfiller --scaffold <carpeta-destino>` desde el CLI — crea un proyecto
`cerver` nuevo desde cero: el esqueleto reutilizable completo (`core/`,
`config/`, `utils/`, autenticación JWT, Swagger UI, `Dockerfile`/
`docker-compose.yml`/`Makefile`, un `.env.example` con placeholders) sin
ningún endpoint de negocio. La carpeta destino tiene que no existir o estar
vacía. `dbfiller` es standalone para esto: el esqueleto viaja embebido en el
binario (`src/boilerplate_zip.h`, ver "Regenerar el esqueleto embebido" más
abajo), no hace falta tener un checkout de `cerver` al lado.

### Simplificación aceptada a propósito

Todo lo anterior corre en el hilo principal de GTK, sin hilo de fondo —
para el uso esperado (Postgres/Docker locales, una operación por vez) es
rápido; el costo es que la ventana no responde mientras cada operación está
en curso. `pump_gtk_events()` entre pasos largos (una línea de compilación,
una línea de `ab`) mitiga esto repintando la UI sin necesitar un hilo
aparte.

## Requisito de setup (una sola vez)

`routes/index.h` y `models/registry.h` necesitan los comentarios-marcador
`/* dbfiller:includes-point */` y `/* dbfiller:routes-point */` (o
`/* dbfiller:models-point */`) para que dbfiller sepa dónde insertar cada
tabla nueva. Ya están agregados en este repo; si algún día se pierden (por
ejemplo, alguien reescribe esos archivos a mano), dbfiller falla con un error
explícito indicando cuál falta.

## Compilar

Requisitos (Linux): `gcc` y las cabeceras de desarrollo de PostgreSQL
(`libpq-dev` en Debian/Ubuntu, o el paquete equivalente de tu distro), y el
binario `unzip` (usado por `--scaffold`/"Crear proyecto nuevo" para
desempaquetar el boilerplate embebido). Para la GUI, además, `libgtk-4-dev`.

```bash
cd tools/dbfiller
make            # build/dbfiller (CLI, no depende de GTK)
make gui        # build/dbfiller-gui (requiere libgtk-4-dev)
make clean
```

### Regenerar el esqueleto embebido

`src/boilerplate_zip.h` es un archivo generado (no editar a mano) con el
esqueleto reutilizable de `cerver` empaquetado como `.zip` embebido —
`--scaffold`/"Crear proyecto nuevo" lo extraen tal cual, sin leer nada del
disco en tiempo de ejecución. Si cambia algo del esqueleto (`core/`,
`config/`, `utils/`, `Dockerfile`, etc. — ver la lista completa en
`tools/pack_boilerplate.py`), hay que regenerarlo y recompilar:

```bash
# desde la raíz del repo cerver
python3 tools/dbfiller/tools/pack_boilerplate.py . tools/dbfiller/src/boilerplate_zip.h
cd tools/dbfiller && make && make gui
```

## Licencia

GPLv3 — ver [LICENSE](LICENSE).
