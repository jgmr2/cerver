# Invariantes de concurrencia

Este documento no explica cómo funciona el servidor (para eso está el
[README](README.md)) — explica las reglas que hay que respetar para no
romperlo de formas que compilan, pasan una prueba manual con `curl`, y
fallan solo bajo carga concurrente real. Cada regla de acá abajo salió de
un bug concreto encontrado en este mismo motor, no de una intuición
teórica.

## 1. `__thread` vs `static` vs `extern`: la regla que más bugs produjo

El modelo es "thread-per-core": 8 hilos (uno por núcleo), cada uno con su
propio anillo `io_uring`, su propio pool de conexiones Postgres, su
propia tabla de rutas. La forma de lograr "cada hilo tiene lo suyo" es
`__thread` en variables de file scope.

**El error fácil de cometer:** declarar esa variable `__thread`
**y además `static`**, dentro de un header (`.h`) que se incluye desde
más de un `.c`.

En C, `static` en un objeto de file scope le da **linkage interno**: cada
unidad de traducción (cada `.c`, no cada hilo) que incluye ese header
obtiene su propia copia privada del símbolo. Esto es independiente de
`__thread` — `__thread` decide cuántas copias hay *por hilo*, `static`
decide cuántas copias hay *por archivo objeto*. Combinados sin querer,
terminás con `N_hilos × N_archivos_c_que_incluyen_el_header` copias en
vez de `N_hilos`.

**El bug real que esto causó:** `conn_keep_alive[]` (`utils/http/http.h`)
empezó como `static __thread unsigned char conn_keep_alive[...]`.
`core/server.c` parseaba el header `Connection` del cliente y escribía
`keep-alive=1` en su copia. Pero el handler de `/api/sakila/films/top`
vive en `controllers/sakila.c` — una unidad de traducción distinta — y
cuando ese archivo llamaba a `send_res_bin()` (definida inline en el
mismo header), leía **su propia copia** de `conn_keep_alive`, siempre en
cero, y mandaba `Connection: close` sin importar lo que el cliente
hubiera pedido. El síntoma no era un crash: era que la segunda request
sobre una conexión keep-alive se quedaba esperando bytes que el servidor
nunca iba a mandar, porque el cliente (correctamente) esperaba que la
conexión siguiera abierta.

**La regla:** cualquier variable `__thread` que necesite ser la MISMA
para todos los `.c` que incluyen el header (que es casi siempre el caso
para estado por hilo) se declara `extern __thread` en el header, y se
define (sin `extern`, sin `static`) en un único `.c`. Ejemplos ya
aplicados en este repo:

| Variable | Declarada en | Definida en |
|---|---|---|
| `pool[]` (conexiones Postgres) | `config/db.h` | `config/db.c` |
| `conn_keep_alive[]` | `utils/http/http.h` | `utils/http/http.c` |
| `route_params[]` / `route_param_count` | `utils/http/router.h` | `utils/http/router.c` |
| `g_shutdown` (no es `__thread`, pero misma logica de linkage) | `utils/events.h` | `main.c` |
| `g_port` (idem, no `__thread`: es de solo lectura tras `main()`) | `core/server.h` | `main.c` |

**Este mismo bug volvió a pasar una segunda vez, en este repo, en la misma
sesión de trabajo:** al agregar soporte de parámetros de path
(`/recursos/:id`), `route_params[]`/`route_param_count` se declararon
`static __thread` por costumbre. No se notó de inmediato porque el primer
handler de prueba (`echo`, en `controllers/home.h`) vive en un *header*
que termina compilado dentro de la unidad de traducción de
`core/server.c` — el mismo lugar donde corre `dispatch()` — así que por
pura casualidad de dónde vivía el código, leía y escribía la misma copia.
El bug se activó recién cuando se agregó `controllers/auth.c` (un `.c`
real, unidad de traducción separada): cualquier handler ahí que hubiera
llamado a `route_param()` habría leído una copia vacía. Se encontró y
arregló *antes* de que un handler real lo disparara, pero la lección es
la misma: **la ausencia de síntomas no prueba que el patrón esté bien
usado** — un handler que hoy vive en un header conveniente puede terminar
en un `.c` propio mañana, y el bug aparece recién ahí.

`routes[]`/`route_count` (`utils/http/router.h`) siguen siendo la única
excepción deliberada a esta regla: tanto la escritura (`init_routes()`)
como la lectura (`dispatch()`) ocurren siempre dentro de `core/server.c`
— nunca hay una segunda unidad de traducción involucrada, así que el bug
de arriba no puede pasar ahí. Si algún día `dispatch()` se invoca desde
otro `.c`, esto deja de ser seguro y hay que aplicar el mismo patrón
`extern`.

**Antes de agregar una variable `__thread` nueva:** preguntate si algún
`.c` *distinto* de donde se escribe podría llegar a leerla (directamente,
o porque alguna función `static inline` que la usa termine inlineada en
otra unidad de traducción). Si la respuesta es "sí" o "no estoy seguro",
usá `extern` + definición única.

## 2. Todo contexto de evento empieza con `event_type_t`

Cualquier puntero que viaja como `user_data` de un SQE (`io_uring_sqe_set_data`)
y vuelve en un CQE (`io_uring_cqe_get_data`) debe empezar su struct con un
campo `event_type_t` (`utils/events.h`). El loop principal
(`core/server.c`) hace:

```c
event_type_t type = *(event_type_t*)data;
```

sin saber todavía qué struct real hay detrás — el cast es válido en C
porque todas las structs de contexto (`accept_ctx_t`, `initial_read_ctx_t`,
`req_t`, `db_t`) tienen ese campo primero. Si agregás un tipo de evento
nuevo, la struct de contexto correspondiente tiene que respetar este
layout, o el dispatch por tipo lee basura.

Un CQE con `user_data == NULL` se descarta sin más (`if (!data) goto next;`)
— es el caso deliberado de un `io_uring_prep_link_timeout` cuyo resultado
no nos interesa individualmente (ver punto 4).

## 3. Ownership de `req_t` (`utils/http/http.h`)

Un `req_t` se `calloc`a una vez por operación HTTP saliente (escribir una
respuesta, servir un archivo) y viaja mutando su campo `type` a través de
varias fases encadenadas: por ejemplo `OP_OPEN_FILE -> OP_STAT_FILE ->
OP_WRITE_HEADER -> OP_READ_FILE -> OP_WRITE_FILE -> ... -> OP_READ_FILE`
(con `cqe->res == 0` como señal de fin de archivo).

Reglas:
- Se libera (`free`) en el **último** paso de cada pipeline posible, nunca
  antes. Si agregás una fase nueva, revisá `http_helper_handle_cqe` para
  encontrar todos los puntos de salida (éxito y error) y asegurate de que
  cada uno libere el `req_t` exactamente una vez.
- Si `owned_result` (un `PGresult*`) está seteado, hay que `PQclear`-lo en
  el mismo punto donde se libera el `req_t` — es la forma en que
  `send_res_pgresult_ref` mantiene vivo un resultado de Postgres hasta
  que termina de escribirse al socket sin copiarlo a un buffer intermedio.
- Un error a mitad de pipeline (`cqe->res < 0`) no debe dejar el
  `client_fd` sin cerrar: el código existente fuerza `type = OP_CLOSE` y
  encadena un `io_uring_prep_close` antes de liberar, en vez de liberar
  directo y perder el fd.

## 4. `IOSQE_IO_LINK` + `io_uring_prep_link_timeout`, no `io_uring_prep_timeout`

El patrón para leer con un timeout (`_rearm_read`, `utils/http/http.h`) es:

```c
io_uring_prep_read(sqe, fd, buf, len, 0);
sqe->flags |= IOSQE_IO_LINK;
/* ... */
io_uring_prep_link_timeout(sqe2, &ts, 0);
```

`io_uring_prep_link_timeout` prepara un `IORING_OP_LINK_TIMEOUT`: cuando
se linkea (`IOSQE_IO_LINK`) a la operación anterior, **corre en paralelo**
con ella y cancela la que sea más lenta de las dos. Es fácil confundirlo
con `io_uring_prep_timeout`, que prepara un timer independiente sin
relación con el linkeo — con esa función, el timeout no arranca a correr
hasta que la operación linkeada ya terminó, o sea que no acota nada. Este
motor tuvo exactamente ese bug: el read inicial no tenía límite real de
tiempo, y una conexión que abría el socket y nunca mandaba nada quedaba
colgada para siempre (slowloris). El fix fue puramente cambiar qué
función de `liburing` se llama, no la lógica alrededor.

Si agregás un nuevo read/write que necesite timeout, replicá este mismo
patrón (`_rearm_read` como referencia), no reinventes uno con
`io_uring_prep_timeout` suelto.

## 5. Ningún syscall bloqueante en el hilo worker — con una excepción documentada

Ninguna función que corre dentro del loop de eventos de un hilo worker
debería bloquear: eso congela a **todas** las conexiones que ese hilo
está atendiendo, no solo la que disparó la llamada. `accept`, `read`,
`write`, `openat2`, `statx`, y el ciclo de vida completo de una consulta
Postgres pasan por `io_uring` de forma asíncrona para respetar esto.

La única excepción deliberada es `reconnect_if_dead` (`config/db.c`):
cuando una conexión Postgres muere (Postgres se reinició, hubo un corte
de red) y se detecta vía `PQstatus() != CONNECTION_OK` justo antes de
mandar una consulta, se reconecta con `PQconnectdb` — síncrono. La
alternativa correcta (una máquina de estados con `PQconnectStart` /
`PQconnectPoll` sobre el mismo anillo `io_uring`) es sustancialmente más
código para un evento que en la práctica es raro; se optó por aceptar el
costo acotado (bloquear ese hilo hasta `connect_timeout=3` segundos, una
vez por conexión caída, no por cada consulta) en vez de la alternativa
que existía antes: una conexión muerta quedaba muerta para siempre y el
pool se degradaba conexión por conexión hasta que alguien reiniciaba el
proceso a mano. Si esto se vuelve un problema real de latencia bajo
caídas de Postgres frecuentes, ahí sí vale la pena escribir la versión
asíncrona completa.

No agregues una segunda excepción a esta regla sin buena razón — cada
llamada bloqueante nueva es un punto donde una operación lenta puede
arrastrar consigo a cientos de conexiones no relacionadas.

## 6. Ownership de `PGresult` en el contrato `cb`

El callback (`cb`, `config/db.h`) que recibe cada consulta async no es
dueño del `PGresult*` por defecto: `handle_db_cqe` lo libera (`PQclear`)
apenas el callback retorna, **salvo que el callback devuelva `1`**, en
cuyo caso la responsabilidad de liberarlo pasa a quien lo recibió (ver
`send_res_pgresult_ref` para el único caso actual de esto, cuando
conviene evitar copiar el resultado a un buffer intermedio).

Si escribís un callback nuevo: devolvé `0` salvo que tengas una razón
concreta para quedarte con el resultado más allá del propio callback, y
si devolvés `1`, asegurate de que el `PGresult` se libere en algún punto
downstream (buscá `PQclear` en el código que lo recibe).

## 6.1. Pasar contexto extra a un callback (`userdata`)

El callback `cb` recibe `(io_uring*, client_fd, PGresult*, void* userdata)`.
`userdata` es lo que se pasó como último argumento a
`db_query_*_async(..., userdata)` al lanzar la consulta — `config/db.c`
solo lo transporta a través de `db_t.userdata` (y de `pending_req_t` si
la consulta se encoló por falta de conexión libre), nunca lo interpreta
ni es dueño de lo que apunta.

Se agregó porque `controllers/auth.c` necesita, en el callback de login,
la contraseña en texto plano que el cliente mandó — dato que no viene en
el `PGresult` (que solo trae el hash guardado) y que no sobrevive
naturalmente hasta que la consulta asíncrona completa. Antes de esto, la
tentación fácil es guardar ese dato en una variable `__thread` "de
paso" — pero eso reintroduce el mismo problema de fondo que en toda la
sección 1: si dos requests distintas están en vuelo en el mismo hilo al
mismo tiempo (interleaving normal de este modelo async), una sola
variable compartida se pisa entre ellas. `userdata` evita el problema
porque viaja atado a la conexión (`db_t`) que sirve *esa* consulta
específica, no a un slot global.

**El patrón, si necesitás pasar contexto a un callback:**
1. `calloc` una struct chica con lo que el callback necesita.
2. Pasarla como `userdata` en `db_query_*_async(...)`.
3. En el callback: castear `userdata`, usarla, y `free()`-la vos mismo
   antes de retornar — en **todos** los caminos de salida del callback
   (éxito y error), no solo el feliz. `config/db.c` nunca la libera.
4. Si `db_query_*_async` falla de forma síncrona (pool agotado, envío
   rechazado), el callback se invoca inline, en el mismo call stack de
   la función que lo lanzó — no asumas que `userdata` sigue viva después
   de esa llamada si el caller la vuelve a tocar (ver `login_user` en
   `controllers/auth.c` para un ejemplo de esto hecho bien: no toca
   `ctx` después de pasarlo).

## 7. Apagado graceful (`g_shutdown`)

`g_shutdown` (`utils/events.h`, definida en `main.c`) es un
`volatile sig_atomic_t`, no `__thread`: es la misma bandera para los 8
hilos, escrita únicamente por el manejador de `SIGTERM`/`SIGINT`. El
protocolo, una vez que un hilo la nota:

1. Deja de re-armar el `accept` (no se aceptan conexiones nuevas).
2. Cualquier conexión que haya entrado en la ventana entre "se pidió el
   apagado" y "dejamos de aceptar" se cierra sin procesar.
3. `_rearm_or_close` deja de re-armar keep-alive: cada conexión existente
   se cierra apenas termina su request en curso, en vez de seguir
   esperando la siguiente.
4. El hilo sigue procesando CQEs (dejando terminar consultas y escrituras
   ya en vuelo) hasta que el anillo queda ocioso o se cumplen
   `SHUTDOWN_GRACE_SECONDS` (5s, `core/server.c`), lo que pase primero.

Si tocás el loop principal de `worker_loop`, cualquier código que decida
"seguir esperando más eventos" tiene que revisar `g_shutdown` — de lo
contrario un hilo puede quedar esperando indefinidamente durante un
apagado que debería ser rápido.

## 8. Antes de tocar el motor: checklist

- ¿Agregaste una variable `__thread` en un header? ¿La escribe un `.c` y
  la lee otro? Si sí: `extern` + definición única (regla 1).
- ¿Agregaste un contexto de evento nuevo? ¿Su primer campo es
  `event_type_t`? (regla 2)
- ¿Agregaste una operación de escritura/lectura nueva sobre un `req_t`?
  ¿Hay un solo `free()` alcanzable desde cada camino de éxito y error?
  (regla 3)
- ¿Necesitás un timeout sobre un read/write? `io_uring_prep_link_timeout`,
  no `io_uring_prep_timeout` (regla 4).
- ¿Estás por llamar algo que pueda bloquear (una libc que no sea
  `_nb`/async, una lectura de archivo sin `io_uring`, etc.) desde dentro
  de `worker_loop` o cualquier callback que corra en su hilo? No, salvo
  que documentes explícitamente por qué es una excepción aceptable
  (regla 5).
- ¿Un callback de consulta async necesita datos que no vienen en el
  `PGresult`? `userdata` (regla 6.1), no una variable `__thread` de paso
  — esa se pisa entre requests interleaved en el mismo hilo.
