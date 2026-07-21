#!/usr/bin/env python3
"""
pack_boilerplate.py - empaqueta el esqueleto reutilizable de cerver (todo
lo que es igual entre proyectos: core/, config/, utils/, docs-ui/, la
autenticacion base, el Dockerfile/compose/Makefile) en un .zip embebido
como arreglo de bytes en un header C, para que dbfiller pueda crear un
proyecto nuevo (--scaffold / "Nuevo proyecto" en la GUI) sin depender de
tener un checkout de cerver al lado del binario.

Uso (desde la raiz del repo cerver):
    python3 tools/dbfiller/tools/pack_boilerplate.py . tools/dbfiller/src/boilerplate_zip.h

Hay que volver a correrlo (y recompilar dbfiller) cada vez que cambie algo
del esqueleto reutilizable - el zip embebido es una foto fija, no se lee
del disco en tiempo de ejecucion.

Que se incluye (deliberadamente, no automatico via "todo menos X"): la
lista esta hardcodeada mas abajo (BOILERPLATE_PATHS) para que agregar un
archivo nuevo al esqueleto sea una decision explicita, no un descuido de
"se colo un archivo que no debia" ni "me olvide de sumarlo".
"""
import io
import os
import sys
import zipfile

BOILERPLATE_PATHS = [
    "config/db.c",
    "config/db.h",
    "controllers/home.h",
    "core/server.c",
    "core/server.h",
    "utils/events.h",
    "utils/json.h",
    "utils/auth/auth.c",
    "utils/auth/auth.h",
    "utils/auth/jwt.c",
    "utils/auth/jwt.h",
    "utils/auth/password.c",
    "utils/auth/password.h",
    "utils/auth/users.c",
    "utils/auth/users.h",
    "utils/http/http.c",
    "utils/http/http.h",
    "utils/http/json_body.h",
    "utils/http/mime.h",
    "utils/http/picohttpparser.c",
    "utils/http/picohttpparser.h",
    "utils/http/router.c",
    "utils/http/router.h",
    "utils/http/static.h",
    "routes/index.h",
    "models/registry.h",
    "models/homeModel.h",
    "docs-ui/favicon-16x16.png",
    "docs-ui/favicon-32x32.png",
    "docs-ui/index.html",
    "docs-ui/openapi.yaml",
    "docs-ui/swagger-ui-bundle.js",
    "docs-ui/swagger-ui.css",
    "docs-ui/swagger-ui-standalone-preset.js",
    "db/init/01_auth_schema.sql",
    "db/init/README.md",
    "public/index.html",
    "public/favicon.svg",
    "main.c",
    "Makefile",
    "Dockerfile",
    "docker-compose.yml",
    ".gitignore",
    "README.md",
]

# .env real NUNCA se embebe (tiene secretos/credenciales de desarrollo del
# checkout que corrio esto) - en su lugar se genera un .env.example fijo,
# con placeholders, directo en el zip.
ENV_EXAMPLE = """# Credenciales - generar valores propios para cada proyecto/entorno nuevo,
# nunca reusar los de otro cliente.
POSTGRES_USER=cambiar
POSTGRES_PASSWORD=cambiar
POSTGRES_DB=cambiar

# Unix Domain Socket compartido con el contenedor de Postgres (ver
# docker-compose.yml, volumen pg_socket) - no lleva host:puerto.
DATABASE_URL=postgres://cambiar:cambiar@/cambiar?host=/var/run/postgresql

# Secreto para firmar/verificar JWT (HS256, ver utils/auth/jwt.c).
# Generar uno nuevo por entorno con: openssl rand -hex 32
JWT_SECRET=cambiar

# Tunables operativos opcionales - si no se definen aca, docker-compose.yml
# les pone el default que se ve al lado de cada una.
# PORT=8080
# SHUTDOWN_GRACE_SECONDS=5
# CACHE_REFRESH_SECONDS=30
# DB_CONNECT_TIMEOUT_SECONDS=3
# JWT_EXPIRES_SECONDS=86400
# PBKDF2_ITERATIONS=100000
# DB_POOL_SIZE=16
# DB_PENDING_QUEUE_SIZE=8192
"""


def build_zip_bytes(repo_root):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel_path in BOILERPLATE_PATHS:
            abs_path = os.path.join(repo_root, rel_path)
            if not os.path.isfile(abs_path):
                raise FileNotFoundError(f"falta '{rel_path}' (esperado en {abs_path})")
            zf.write(abs_path, arcname=rel_path)
        zf.writestr(".env.example", ENV_EXAMPLE)
    return buf.getvalue()


def emit_header(data, out_path):
    with open(out_path, "w") as f:
        f.write("/*\n")
        f.write(" * boilerplate_zip.h - GENERADO por tools/pack_boilerplate.py. No editar a mano.\n")
        f.write(" *\n")
        f.write(" * Contiene el esqueleto reutilizable de cerver (ver BOILERPLATE_PATHS en el\n")
        f.write(" * script) empaquetado como .zip y embebido como arreglo de bytes, para que\n")
        f.write(" * scaffold_new_project() (scaffold.c) pueda crear un proyecto nuevo sin\n")
        f.write(" * depender de un checkout de cerver al lado del binario. Volver a correr el\n")
        f.write(" * script (y recompilar) cada vez que cambie algo del esqueleto.\n")
        f.write(" */\n")
        f.write("#ifndef DBFILLER_BOILERPLATE_ZIP_H\n")
        f.write("#define DBFILLER_BOILERPLATE_ZIP_H\n\n")
        f.write(f"static const unsigned long DBFILLER_BOILERPLATE_ZIP_LEN = {len(data)}UL;\n")
        f.write("static const unsigned char DBFILLER_BOILERPLATE_ZIP[] = {\n")
        for i in range(0, len(data), 20):
            chunk = data[i:i + 20]
            f.write("    " + ",".join(str(b) for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write("#endif\n")


def main():
    if len(sys.argv) != 3:
        print(f"Uso: {sys.argv[0]} <raiz-repo-cerver> <ruta-header-salida>", file=sys.stderr)
        return 1

    repo_root, out_path = sys.argv[1], sys.argv[2]
    data = build_zip_bytes(repo_root)
    emit_header(data, out_path)
    print(f"OK: {out_path} ({len(data)} bytes de zip, {len(BOILERPLATE_PATHS) + 1} archivos)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
