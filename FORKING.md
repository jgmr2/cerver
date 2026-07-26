# Crear el backend de un cliente nuevo, y mantenerlo al día

Este repo (`cerver`) es la plantilla base. Cada cliente es su propio
repo privado en GitHub, clonado a partir de acá, con este repo agregado
como remoto `upstream` — así un fix del framework se puede traer
después sin perder la personalización de cada cliente.

## Crear el fork de un cliente nuevo

1. Crear un repo privado nuevo y vacío en GitHub (ej. `cliente-acme-backend`).
2. Clonar `cerver` y reacomodar los remotos:
   ```bash
   git clone git@github.com:tu-org/cerver.git cliente-acme-backend
   cd cliente-acme-backend
   git remote rename origin upstream
   git remote add origin git@github.com:tu-org/cliente-acme-backend.git
   git push -u origin main
   ```
3. Agregar el esquema propio del cliente en `db/init/`, correr
   `python3 tools/dbfiller/generate.py`, personalizar lo que haga falta
   (ver `tools/dbfiller/README.md`), commitear y pushear a `origin` — a
   `upstream` nunca se pushea desde acá, es de solo lectura.

De acá en más, `origin` = el repo del cliente (donde vive su código),
`upstream` = este repo (de donde vienen los fixes de framework).

## Traer un fix del framework a un cliente ya andando

```bash
./tools/sync-upstream.sh
```

Trae lo nuevo de `upstream` y muestra qué commits hay y qué archivos
tocan, sin aplicar nada todavía. Con esa info:

- **Fix que no toca nada personalizado** (el caso común — los fixes de
  framework viven en `src/core/`, `src/config/`, `src/utils/`, no en lo
  que genera `dbfiller` ni en `db/init/`):
  ```bash
  git merge upstream/main        # todo lo nuevo
  # o, para un checkpoint puntual (ver CHANGELOG.md):
  git merge upstream/v1.2.0
  ```
- **Un fix puntual, sin traer el resto**:
  ```bash
  git cherry-pick <hash>
  ```

Después, pushear a `origin` como cualquier otro cambio.

## Dónde esperar conflictos

- `src/routes/index.h`, `src/models/registry.h`, `docs-ui/openapi.yaml`
  — tienen bloques generados por tabla (marcados
  `dbfiller:<tabla>:...`) intercalados con la parte fija del
  framework. Esos bloques son tuyos (dbfiller los generó a partir de
  TU esquema); un merge de upstream nunca debería tocarlos porque el
  repo base no tiene ninguna tabla de negocio commiteada — si aparece
  un conflicto ahí, probablemente sea en la parte fija de alrededor,
  no en tu bloque.
- `src/utils/auth/auth_model.*`, `src/utils/auth/auth.c` — generados a
  partir de tu esquema de auth. Mismo razonamiento: upstream no trae
  ningún esquema propio, así que no debería tocarlos.
- Si personalizaste a mano un archivo que normalmente es genérico
  (`src/core/`, `src/config/`, `src/utils/` fuera de `auth_model.*`),
  ahí sí puede haber un conflicto real que solo vos podés resolver.

## Versionado de `cerver` (este repo)

Tags `vMAJOR.MINOR.PATCH` marcan checkpoints estables — ver
[`CHANGELOG.md`](CHANGELOG.md) para qué cambió en cada uno. Preferir
mergear/cherry-pickear contra un tag puntual en vez de perseguir `main`
a ciegas: da control de CUÁNDO cada cliente recibe una actualización.
