#!/usr/bin/env python3
"""
generate.py - genera src/controllers/<tabla>.c/.h y src/models/<tabla>.c/.h
con endpoints CRUD (list/get/create/update/delete) a partir de los
"CREATE TABLE" de db/init/*.sql, y registra cada tabla en
src/routes/index.h y src/models/registry.h.

Reemplaza al viejo generador en C (tools/dbfiller/src/*.c, eliminado) que
introspeccionaba una conexion Postgres EN VIVO via libpq: el "esquema" es
directamente el texto ya commiteado en db/init/*.sql, asi que generar el
CRUD no necesita ninguna base corriendo -- ver tools/dbfiller/README.md
para el resto del contexto (que se genera, que NO, limitaciones).

Paso MANUAL (no corre en `docker build`, ver README): se corre a mano
parado en la raiz del repo destino, se revisa el diff, y el resultado se
commitea como cualquier otro cambio -- asi el codigo generado se puede
editar despues a mano para agregarle logica de negocio sin que una
regeneracion futura lo pise: cada archivo generado lleva, ademas del
marcador de la primera linea, un hash del contenido que le sigue (ver
_write_file/_file_generation_state); si al volver a correr dbfiller ese
hash ya no coincide con el contenido en disco, alguien lo edito despues
de la ultima generacion y hace falta --force para sobreescribirlo (el
marcador solo, sin el hash, no alcanzaria para distinguir "nunca se
toco" de "se genero una vez y despues se le agrego logica a mano").

Uso:
    python3 tools/dbfiller/generate.py [--repo-root .] [--schema PATRON ...] [--force]

Requiere el paquete "sqlparse" (ver requirements.txt) -- se usa solo para
separar el archivo .sql en statements individuales (respeta ';' dentro de
strings/comentarios, que un split() ingenuo no respeta); el resto del
parseo (columnas, tipos, constraints) es manual porque sqlparse no da un
AST de columnas listo para usar.
"""
import argparse
import glob
import hashlib
import os
import re
import sys
from dataclasses import dataclass, field

import sqlparse

MARKER = "/* Generado por dbfiller -- ver tools/dbfiller. No editar a mano si vas a re-generar. */"

# El codigo que se replica en cada proyecto nuevo (controllers/, models/,
# routes/, config/, core/, utils/, main.c, Makefile) vive en src/ dentro
# del repo destino -- separado de db/ (esquema/datos, se queda en la
# raiz) y de las herramientas complementarias (tools/, tests/, tampoco en
# src/). Ver README.md de la raiz.
SRC_DIR = "src"

# ---- columnas sensibles: excluidas de toda tabla, sin importar su nombre
# real (agnostico a que tabla sea). Mismas needles que ya usaba
# is_sensitive_column en el viejo codegen.c -- heuristica, no garantia:
# revisar igual el diff antes de compilar. ----
SENSITIVE_NEEDLES = ("password", "passwd", "hash", "secret", "token", "api_key", "apikey", "credential")


def is_sensitive_column(name: str) -> bool:
    lower = name.lower()
    return any(needle in lower for needle in SENSITIVE_NEEDLES)


class GenerateError(Exception):
    pass


# ============================================================
# Modelo de datos (equivalente a schema.h)
# ============================================================

@dataclass
class Column:
    name: str
    data_type: str
    nullable: bool
    has_default: bool
    is_pk: bool = False
    is_unique: bool = False


@dataclass
class Table:
    name: str
    columns: list = field(default_factory=list)
    source_file: str = ""

    @property
    def pk_index(self):
        for i, c in enumerate(self.columns):
            if c.is_pk:
                return i
        return -1


# ============================================================
# Parseo de "CREATE TABLE" (reemplaza introspect.c)
#
# Subconjunto de DDL estandar de Postgres, no cualquier SQL valido --
# mismo criterio de "documentado, no exhaustivo" que ya tenia
# introspect.c (ej. solo PK de una columna). Tipos multi-palabra
# soportados: DOUBLE PRECISION, CHARACTER VARYING(n),
# TIMESTAMP/TIME [(n)] WITH|WITHOUT TIME ZONE.
# ============================================================

_CREATE_TABLE_RE = re.compile(
    r"create\s+table\s+(if\s+not\s+exists\s+)?\"?(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)\"?\s*\(",
    re.IGNORECASE,
)
_CONSTRAINT_FIRST_WORDS = ("primary", "unique", "foreign", "check", "constraint")
_TIMESTAMP_SUFFIXES = ("with time zone", "without time zone")

# PK agregada por separado con "ALTER TABLE [ONLY] [esquema.]tabla ADD
# CONSTRAINT <nombre> PRIMARY KEY (...)" -- el formato que emite pg_dump
# por defecto (nunca inline en el CREATE TABLE). Sin este segundo patron,
# CUALQUIER esquema exportado con pg_dump se salteaba entero (todas sus
# tablas "sin PK"), confirmado con postgres-sakila-schema.sql.
_ALTER_PK_RE = re.compile(
    r"alter\s+table\s+(only\s+)?(?:[a-zA-Z_][a-zA-Z0-9_]*\.)?\"?(?P<table>[a-zA-Z_][a-zA-Z0-9_]*)\"?\s+"
    r"add\s+constraint\s+\S+\s+primary\s+key\s*\((?P<cols>[^)]+)\)",
    re.IGNORECASE,
)

# Mismo formato pg_dump que _ALTER_PK_RE, pero para "ADD CONSTRAINT ...
# UNIQUE (...)" -- necesario para detectar la tabla de autenticacion (ver
# find_auth_table): la columna de identidad (email/username) suele venir
# marcada UNIQUE por un ALTER TABLE separado en un dump real, no siempre
# inline en el CREATE TABLE.
_ALTER_UNIQUE_RE = re.compile(
    r"alter\s+table\s+(only\s+)?(?:[a-zA-Z_][a-zA-Z0-9_]*\.)?\"?(?P<table>[a-zA-Z_][a-zA-Z0-9_]*)\"?\s+"
    r"add\s+constraint\s+\S+\s+unique\s*\((?P<cols>[^)]+)\)",
    re.IGNORECASE,
)


def _strip_sql_comments(text: str) -> str:
    """Reemplaza comentarios SQL ('-- ...' hasta fin de linea, '/' + '* ... *' + '/')
    por espacios (preserva offsets y saltos de linea), respetando strings
    'con comillas' -- un '--' o '/*' dentro de un literal no es un
    comentario. Sin esto, un comentario metido entre columnas (comun en
    los .sql de este repo, ver db/init/01_auth_schema.sql) se colaba como
    si fuera una columna mas al partir por comas."""
    out = []
    i, n = 0, len(text)
    in_string = False
    while i < n:
        c = text[i]
        if in_string:
            out.append(c)
            if c == "'" and not (i + 1 < n and text[i + 1] == "'"):
                in_string = False
            i += 1
            continue
        if c == "'":
            in_string = True
            out.append(c)
            i += 1
            continue
        if text[i:i + 2] == "--":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
            continue
        if text[i:i + 2] == "/*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _find_matching_paren(text: str, open_idx: int) -> int:
    """Indice de ')' que cierra el '(' en open_idx, respetando strings 'con comillas'."""
    depth = 0
    in_string = False
    i = open_idx
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "'":
                if i + 1 < len(text) and text[i + 1] == "'":
                    i += 1
                else:
                    in_string = False
        elif c == "'":
            in_string = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top_level(text: str, sep: str = ",") -> list:
    """Divide text por 'sep' ignorando comas dentro de parentesis o strings."""
    parts = []
    depth = 0
    in_string = False
    start = 0
    i = 0
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "'":
                if i + 1 < len(text) and text[i + 1] == "'":
                    i += 1
                else:
                    in_string = False
        elif c == "'":
            in_string = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
        i += 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip()]


def _extract_type(rest: str):
    """A partir de 'rest' (todo lo que sigue al nombre de columna), consume
    el tipo (posiblemente multi-palabra + '(args)') y devuelve
    (type_str, remainder) con remainder = el resto de constraints."""
    rest = rest.lstrip()
    m = re.match(r"[A-Za-z_]+", rest)
    if not m:
        return "", rest
    base = m.group(0)
    pos = m.end()
    base_lower = base.lower()
    type_str = base

    if base_lower in ("timestamp", "time"):
        paren_m = re.match(r"\s*\(\s*\d+\s*\)", rest[pos:])
        if paren_m:
            type_str += rest[pos:pos + paren_m.end()]
            pos += paren_m.end()
        for suffix in _TIMESTAMP_SUFFIXES:
            suffix_re = r"\s+" + re.escape(suffix).replace(r"\ ", r"\s+")
            suffix_m = re.match(suffix_re, rest[pos:], re.IGNORECASE)
            if suffix_m:
                type_str += " " + suffix
                pos += suffix_m.end()
                break
        return type_str, rest[pos:]

    if base_lower == "double":
        prec_m = re.match(r"\s+precision", rest[pos:], re.IGNORECASE)
        if prec_m:
            type_str += " precision"
            pos += prec_m.end()
        return type_str, rest[pos:]

    if base_lower == "character":
        var_m = re.match(r"\s+varying", rest[pos:], re.IGNORECASE)
        if var_m:
            type_str += " varying"
            pos += var_m.end()

    paren_m = re.match(r"\s*\([^)]*\)", rest[pos:])
    if paren_m:
        type_str += rest[pos:pos + paren_m.end()]
        pos += paren_m.end()

    return type_str, rest[pos:]


def _parse_column(item: str) -> Column:
    m = re.match(r'"?(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)"?\s+(?P<rest>.+)', item, re.DOTALL)
    if not m:
        raise GenerateError(f"no se pudo parsear la columna: {item!r}")
    name = m.group("name")
    type_str, rest = _extract_type(m.group("rest"))

    is_pk = bool(re.search(r"\bprimary\s+key\b", rest, re.IGNORECASE))
    is_unique = bool(re.search(r"\bunique\b", rest, re.IGNORECASE))
    not_null = bool(re.search(r"\bnot\s+null\b", rest, re.IGNORECASE))
    has_generated_identity = bool(re.search(r"\bgenerated\b.*\bas\s+identity\b", rest, re.IGNORECASE))
    has_default = (
        bool(re.search(r"\bdefault\b", rest, re.IGNORECASE))
        or type_str.lower() in ("serial", "bigserial", "smallserial")
        or has_generated_identity
    )
    nullable = not (not_null or is_pk)

    return Column(name=name, data_type=type_str, nullable=nullable, has_default=has_default, is_pk=is_pk, is_unique=is_unique)


def _parse_create_table(stmt: str, source_file: str) -> Table:
    m = _CREATE_TABLE_RE.search(stmt)
    if not m:
        return None
    name = m.group("name")
    open_idx = m.end() - 1
    close_idx = _find_matching_paren(stmt, open_idx)
    if close_idx < 0:
        raise GenerateError(f"'{name}' ({source_file}): parentesis de columnas sin cerrar")

    body = stmt[open_idx + 1:close_idx]
    items = _split_top_level(body)

    table = Table(name=name, source_file=source_file)
    table_level_pk = []
    table_level_unique = []

    for item in items:
        first_word = item.strip().split(None, 1)[0].lower().strip('"')
        if first_word in _CONSTRAINT_FIRST_WORDS:
            pk_m = re.search(r"primary\s+key\s*\(([^)]+)\)", item, re.IGNORECASE)
            if pk_m:
                table_level_pk = [c.strip().strip('"') for c in pk_m.group(1).split(",")]
            uq_m = re.search(r"unique\s*\(([^)]+)\)", item, re.IGNORECASE)
            if uq_m:
                table_level_unique.append([c.strip().strip('"') for c in uq_m.group(1).split(",")])
            continue
        table.columns.append(_parse_column(item))

    if table_level_pk:
        _mark_pk(table, table_level_pk)
    for cols in table_level_unique:
        _mark_unique(table, cols)

    return table


def _mark_pk(table: Table, col_names: list):
    """Marca is_pk=True (y fuerza nullable=False -- una PK nunca acepta
    NULL en Postgres sin importar como se haya declarado la columna) para
    col_names si es una sola columna. PK compuesta (> 1 columna): se deja
    sin marcar -- pk_index da -1 y el llamador la saltea, mismo criterio
    que ya tenia introspect.c."""
    if len(col_names) != 1:
        return
    for c in table.columns:
        if c.name == col_names[0]:
            c.is_pk = True
            c.nullable = False


def _mark_unique(table: Table, col_names: list):
    """Igual que _mark_pk pero para UNIQUE: solo de una sola columna nos
    interesa (identifica candidatos a columna de login en
    find_auth_table) -- un UNIQUE compuesto (ej. UNIQUE(fk_role,
    fk_permission)) no vuelve a ninguna columna individual 'unica' por si
    sola, se ignora a proposito."""
    if len(col_names) != 1:
        return
    for c in table.columns:
        if c.name == col_names[0]:
            c.is_unique = True


def parse_schema_files(paths) -> list:
    statements = []
    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        for raw_stmt in sqlparse.split(text):
            stmt = _strip_sql_comments(raw_stmt).strip()
            if stmt:
                statements.append((stmt, os.path.basename(path)))

    tables = []
    tables_by_name = {}
    for stmt, source in statements:
        if not re.search(r"create\s+table", stmt, re.IGNORECASE):
            continue
        table = _parse_create_table(stmt, source)
        if table and table.columns:
            tables.append(table)
            tables_by_name[table.name] = table

    # Segunda pasada: PK agregada por separado (ALTER TABLE ... ADD
    # CONSTRAINT ... PRIMARY KEY), el formato que emite pg_dump -- tiene
    # que ser una pasada aparte porque puede aparecer en cualquier orden
    # relativo a su CREATE TABLE (pg_dump las pone todas al final).
    for stmt, _source in statements:
        if not re.search(r"alter\s+table", stmt, re.IGNORECASE):
            continue
        pk_m = _ALTER_PK_RE.search(stmt)
        if pk_m:
            table = tables_by_name.get(pk_m.group("table"))
            if table:
                cols = [c.strip().strip('"') for c in pk_m.group("cols").split(",")]
                _mark_pk(table, cols)
        uq_m = _ALTER_UNIQUE_RE.search(stmt)
        if uq_m:
            table = tables_by_name.get(uq_m.group("table"))
            if table:
                cols = [c.strip().strip('"') for c in uq_m.group("cols").split(",")]
                _mark_unique(table, cols)

    return tables


# ============================================================
# Clasificacion de columnas (equivalente a classify() en codegen.c)
# ============================================================

def classify(table: Table):
    all_cols, insertable, updatable, skipped = [], [], [], []
    for c in table.columns:
        if is_sensitive_column(c.name):
            skipped.append(c)
            continue
        all_cols.append(c)
        if not c.has_default:
            insertable.append(c)
            if not c.is_pk:
                updatable.append(c)
    return all_cols, insertable, updatable, skipped


def to_pascal(name: str) -> str:
    return name[0].upper() + name[1:] if name else name


# ============================================================
# Deteccion de la tabla de autenticacion (agnostica al nombre de tabla)
#
# Cualquier tabla que tenga PK de una sola columna + una columna de
# identidad (llamada exactamente "email" o "username", UNIQUE y NOT
# NULL) + exactamente una columna sensible tipo password (ver
# is_sensitive_column) sirve como backing de autenticacion -- register/
# login/me se generan a partir de ELLA, sin ningun nombre de tabla
# hardcodeado (ver tools/dbfiller/README.md, seccion "Autenticacion").
# ============================================================

IDENTITY_COLUMN_NAMES = ("email", "username")


@dataclass
class AuthCandidate:
    table: Table
    identity_col: "Column"
    password_col: "Column"


def _find_identity_column(table: Table):
    for c in table.columns:
        if c.name.lower() in IDENTITY_COLUMN_NAMES and c.is_unique and not c.nullable:
            return c
    return None


def find_auth_table(tables: list, auth_table_name: str = None):
    """Retorna un AuthCandidate o None. Si auth_table_name viene dado
    (--auth-table), se usa esa tabla puntual (debe existir y calificar);
    si no, se evaluan todas y hace falta que califique EXACTAMENTE UNA
    -- mas de un candidato es un GenerateError (nunca se elige "la
    primera" en silencio: cual tabla es la de auth es una decision de
    seguridad, no un accidente de orden de archivos)."""
    if auth_table_name is not None:
        table = next((t for t in tables if t.name == auth_table_name), None)
        if not table:
            raise GenerateError(f"--auth-table '{auth_table_name}': no se encontro esa tabla en el esquema")
        if table.pk_index < 0:
            raise GenerateError(f"--auth-table '{auth_table_name}': no tiene una PRIMARY KEY de una sola columna")
        identity_col = _find_identity_column(table)
        if not identity_col:
            raise GenerateError(
                f"--auth-table '{auth_table_name}': no tiene una columna 'email'/'username' UNIQUE NOT NULL"
            )
        sensitive = [c for c in table.columns if is_sensitive_column(c.name)]
        if len(sensitive) != 1:
            raise GenerateError(
                f"--auth-table '{auth_table_name}': se esperaba exactamente una columna tipo password, "
                f"se encontraron {len(sensitive)} ({', '.join(c.name for c in sensitive)})"
            )
        return AuthCandidate(table=table, identity_col=identity_col, password_col=sensitive[0])

    candidates = []
    for table in tables:
        if table.pk_index < 0:
            continue
        identity_col = _find_identity_column(table)
        if not identity_col:
            continue
        sensitive = [c for c in table.columns if is_sensitive_column(c.name)]
        if len(sensitive) != 1:
            continue
        candidates.append(AuthCandidate(table=table, identity_col=identity_col, password_col=sensitive[0]))

    if not candidates:
        return None
    if len(candidates) > 1:
        names = ", ".join(c.table.name for c in candidates)
        raise GenerateError(
            f"mas de una tabla califica como tabla de autenticacion ({names}) -- "
            "desambigua con --auth-table <nombre>, o renombra/saca del --schema la que no corresponda"
        )
    return candidates[0]


# ============================================================
# Generacion de codigo (puerto directo de codegen.c)
# ============================================================

def _join_names(cols, sep=", "):
    return sep.join(c.name for c in cols)


def _join_placeholders(count, start):
    return ", ".join(f"${start + i}" for i in range(count))


def _join_set_clause(cols):
    return ", ".join(f"{c.name} = ${i + 1}" for i, c in enumerate(cols))


def gen_model_h(table, TABLE, Table_, has_update) -> str:
    out = [MARKER, ""]
    out.append(f"/*\n * models/{table.name}.h - modelo CRUD generado para la tabla '{table.name}'\n */")
    out.append(f"#ifndef MODELS_{TABLE}_H")
    out.append(f"#define MODELS_{TABLE}_H")
    out.append("")
    out.append("#include <liburing.h>")
    out.append('#include "../config/db.h"')
    out.append("")
    out.append(f"void {table.name}_register(void);")
    out.append("")
    out.append(f"void {Table_}_list_async(struct io_uring *r, int client_fd, cb callback, void *userdata);")
    out.append(f"void {Table_}_get_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata);")
    out.append(f"void {Table_}_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata);")
    if has_update:
        out.append(f"void {Table_}_update_async(struct io_uring *r, int client_fd, const char *const *values, const char *id, cb callback, void *userdata);")
    out.append(f"void {Table_}_delete_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata);")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def gen_model_c(table, cl, TABLE, Table_, has_update) -> str:
    all_cols, insertable, updatable, skipped = cl
    pk = table.columns[table.pk_index].name

    public_cols = _join_names(all_cols)
    insertable_cols = _join_names(insertable)
    placeholders = _join_placeholders(len(insertable), 1)
    set_clause = _join_set_clause(updatable)

    out = [MARKER, f'#include "{table.name}.h"', ""]
    out.append(f'#define {TABLE}_STMT_LIST   "{table.name}_list"')
    out.append(f'#define {TABLE}_STMT_GET    "{table.name}_get"')
    out.append(f'#define {TABLE}_STMT_CREATE "{table.name}_create"')
    if has_update:
        out.append(f'#define {TABLE}_STMT_UPDATE "{table.name}_update"')
    out.append(f'#define {TABLE}_STMT_DELETE "{table.name}_delete"')
    out.append("")

    if skipped:
        skipped_names = _join_names(skipped)
        out.append(
            "/* Excluidas automaticamente por nombre (password/hash/secret/token/\n"
            "   api_key, sin importar mayusculas, ver is_sensitive_column en\n"
            "   tools/dbfiller/generate.py) - no se leen ni se aceptan via estos\n"
            f"   endpoints: {skipped_names}.\n"
            f"   Si de verdad hace falta exponerlas, agregalas a mano a {TABLE}_COLUMNS\n"
            "   (list/get) y a los INSERT/UPDATE de este archivo (create/update). */"
        )
    out.append(
        "/* Columnas expuestas via JSON en list/get/create/update. Si esta\n"
        "   tabla tiene otras columnas sensibles que el filtro automatico no\n"
        "   detecto, sacalas de aca antes de compilar. */"
    )
    out.append(f'#define {TABLE}_COLUMNS "{public_cols}"')
    out.append("")

    out.append(f"void {table.name}_register(void) {{")
    out.append(
        f"    db_register_prepared({TABLE}_STMT_LIST,\n"
        f'        "SELECT COALESCE(json_agg(row_to_json(t)), \'[]\'::json) FROM (SELECT " {TABLE}_COLUMNS " FROM {table.name} ORDER BY {pk} LIMIT 100) t;");\n'
    )
    out.append(
        f"    db_register_prepared({TABLE}_STMT_GET,\n"
        f'        "SELECT row_to_json(t) FROM (SELECT " {TABLE}_COLUMNS " FROM {table.name} WHERE {pk} = $1) t;");\n'
    )
    if insertable:
        out.append(
            f"    db_register_prepared({TABLE}_STMT_CREATE,\n"
            f'        "WITH ins AS (INSERT INTO {table.name} ({insertable_cols}) VALUES ({placeholders}) RETURNING " {TABLE}_COLUMNS ") SELECT row_to_json(ins) FROM ins;");\n'
        )
    else:
        out.append(
            f"    db_register_prepared({TABLE}_STMT_CREATE,\n"
            f'        "WITH ins AS (INSERT INTO {table.name} DEFAULT VALUES RETURNING " {TABLE}_COLUMNS ") SELECT row_to_json(ins) FROM ins;");\n'
        )
    if has_update:
        out.append(
            f"    db_register_prepared({TABLE}_STMT_UPDATE,\n"
            f'        "WITH upd AS (UPDATE {table.name} SET {set_clause} WHERE {pk} = ${len(updatable) + 1} RETURNING " {TABLE}_COLUMNS ") SELECT row_to_json(upd) FROM upd;");\n'
        )
    out.append(f'    db_register_prepared({TABLE}_STMT_DELETE,\n        "DELETE FROM {table.name} WHERE {pk} = $1;");')
    out.append("}")
    out.append("")

    out.append(f"void {Table_}_list_async(struct io_uring *r, int client_fd, cb callback, void *userdata) {{")
    out.append(f"    db_query_prepared_async(r, client_fd, {TABLE}_STMT_LIST, callback, userdata);")
    out.append("}")
    out.append("")

    out.append(f"void {Table_}_get_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata) {{")
    out.append("    const char *params[1] = { id };")
    out.append(f"    db_query_prepared_params_async(r, client_fd, {TABLE}_STMT_GET, 1, params, 0, callback, userdata);")
    out.append("}")
    out.append("")

    out.append(f"void {Table_}_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata) {{")
    if insertable:
        out.append(f"    db_query_prepared_params_async(r, client_fd, {TABLE}_STMT_CREATE, {len(insertable)}, values, 0, callback, userdata);")
    else:
        out.append("    (void)values;")
        out.append(f"    db_query_prepared_async(r, client_fd, {TABLE}_STMT_CREATE, callback, userdata);")
    out.append("}")
    out.append("")

    if has_update:
        out.append(f"void {Table_}_update_async(struct io_uring *r, int client_fd, const char *const *values, const char *id, cb callback, void *userdata) {{")
        out.append(f"    const char *params[{len(updatable) + 1}];")
        out.append(f"    for (int i = 0; i < {len(updatable)}; i++) params[i] = values[i];")
        out.append(f"    params[{len(updatable)}] = id;")
        out.append(f"    db_query_prepared_params_async(r, client_fd, {TABLE}_STMT_UPDATE, {len(updatable) + 1}, params, 0, callback, userdata);")
        out.append("}")
        out.append("")

    out.append(f"void {Table_}_delete_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata) {{")
    out.append("    const char *params[1] = { id };")
    out.append(f"    db_query_prepared_params_async(r, client_fd, {TABLE}_STMT_DELETE, 1, params, 0, callback, userdata);")
    out.append("}")

    return "\n".join(out) + "\n"


def gen_controller_h(table, TABLE, has_update) -> str:
    out = [MARKER, ""]
    out.append(f"/*\n * controllers/{table.name}.h - endpoints CRUD generados para la tabla '{table.name}'\n */")
    out.append(f"#ifndef CONTROLLERS_{TABLE}_H")
    out.append(f"#define CONTROLLERS_{TABLE}_H")
    out.append("")
    out.append("#include <liburing.h>")
    out.append("")
    out.append(f"void list_{table.name}(struct io_uring *r, int fd, const char *m, const char *b);")
    out.append(f"void get_{table.name}(struct io_uring *r, int fd, const char *m, const char *b);")
    out.append(f"void create_{table.name}(struct io_uring *r, int fd, const char *m, const char *b);")
    if has_update:
        out.append(f"void update_{table.name}(struct io_uring *r, int fd, const char *m, const char *b);")
    out.append(f"void delete_{table.name}(struct io_uring *r, int fd, const char *m, const char *b);")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def _gen_body_parse_block(cols) -> str:
    out = [f"    jsmntok_t tokens[{len(cols) * 2 + 4}];"]
    out.append("    const char *json_body = NULL;")
    out.append("    int ntok = http_parse_json_body(b, tokens, sizeof(tokens) / sizeof(tokens[0]), &json_body);")
    out.append('    if (ntok < 1 || !json_body) { send_res(r, fd, "400 Bad Request", "text/plain", "400"); return; }')
    out.append("")
    for c in cols:
        out.append(f"    char buf_{c.name}[256];")
        out.append(f'    int st_{c.name} = json_body_field(json_body, tokens, ntok, "{c.name}", buf_{c.name}, sizeof(buf_{c.name}));')
        out.append(f'    if (st_{c.name} < 0) {{ send_res(r, fd, "400 Bad Request", "text/plain", "400"); return; }}')
    out.append("")
    out.append(f"    const char *values[{len(cols)}];")
    for i, c in enumerate(cols):
        out.append(f"    if (st_{c.name} == 0 || st_{c.name} == 2) {{")
        if c.nullable:
            out.append(f"        values[{i}] = NULL;")
        else:
            out.append('        send_res(r, fd, "400 Bad Request", "text/plain", "400"); return;')
        out.append(f"    }} else {{\n        values[{i}] = buf_{c.name};\n    }}")
    return "\n".join(out)


def _gen_data_body_and_send(use_201: bool) -> str:
    out = [
        "    const char *json = PQgetvalue(res, 0, 0);",
        "    size_t len = strlen(json);",
        "    char *body = malloc(len + 16);",
        '    if (!body) { send_res(r, client_fd, "500 Internal Server Error", "text/plain", "500"); return 0; }',
        '    snprintf(body, len + 16, "{\\"data\\":%s}", json);',
    ]
    if use_201:
        out.append('    send_res(r, client_fd, "201 Created", "application/json", body);')
    else:
        out.append("    res_json(r, client_fd, body);")
    out.append("    free(body);")
    out.append("    return 0;")
    out.append("}")
    out.append("")
    return "\n".join(out)


def gen_controller_c(table, cl, Table_, has_update) -> str:
    all_cols, insertable, updatable, skipped = cl
    name = table.name

    out = [MARKER, f'#include "{name}.h"', ""]
    out.append("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n")
    out.append(f'#include "../config/db.h"')
    out.append(f'#include "../models/{name}.h"')
    out.append('#include "../utils/http/router.h"')
    out.append('#include "../utils/http/json_body.h"')
    out.append("")

    # list
    out.append(f"static int on_{name}_list(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {{")
    out.append("    (void)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
    out.append(f'        res_json(r, client_fd, "{{\\"error\\":\\"{name} query failed\\"}}");')
    out.append("        return 0;")
    out.append("    }")
    out.append(_gen_data_body_and_send(False))
    out.append(f"void list_{name}(struct io_uring *r, int fd, const char *m, const char *b) {{")
    out.append("    (void)m; (void)b;")
    out.append(f"    {Table_}_list_async(r, fd, on_{name}_list, NULL);")
    out.append("}")
    out.append("")

    # get
    out.append(f"static int on_{name}_get(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {{")
    out.append("    (void)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
    out.append('        send_res(r, client_fd, "404 Not Found", "text/plain", "404");')
    out.append("        return 0;")
    out.append("    }")
    out.append(_gen_data_body_and_send(False))
    out.append(f"void get_{name}(struct io_uring *r, int fd, const char *m, const char *b) {{")
    out.append("    (void)m; (void)b;")
    out.append('    const char *id = route_param("id");')
    out.append('    if (!id) { send_res(r, fd, "400 Bad Request", "text/plain", "400"); return; }')
    out.append(f"    {Table_}_get_async(r, fd, id, on_{name}_get, NULL);")
    out.append("}")
    out.append("")

    # create
    out.append(f"static int on_{name}_created(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {{")
    out.append("    (void)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
    out.append('        send_res(r, client_fd, "409 Conflict", "text/plain", "409");')
    out.append("        return 0;")
    out.append("    }")
    out.append(_gen_data_body_and_send(True))
    out.append(f"void create_{name}(struct io_uring *r, int fd, const char *m, const char *b) {{")
    out.append("    (void)m;")
    if insertable:
        out.append(_gen_body_parse_block(insertable))
        out.append("")
        out.append(f"    {Table_}_create_async(r, fd, values, on_{name}_created, NULL);")
        out.append("}")
    else:
        out.append("    (void)b;")
        out.append(f"    {Table_}_create_async(r, fd, NULL, on_{name}_created, NULL);")
        out.append("}")
    out.append("")

    # update
    if has_update:
        out.append(f"static int on_{name}_updated(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {{")
        out.append("    (void)userdata;")
        out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
        out.append('        send_res(r, client_fd, "404 Not Found", "text/plain", "404");')
        out.append("        return 0;")
        out.append("    }")
        out.append(_gen_data_body_and_send(False))
        out.append(f"void update_{name}(struct io_uring *r, int fd, const char *m, const char *b) {{")
        out.append("    (void)m;")
        out.append('    const char *id = route_param("id");')
        out.append('    if (!id) { send_res(r, fd, "400 Bad Request", "text/plain", "400"); return; }')
        out.append("")
        out.append(_gen_body_parse_block(updatable))
        out.append("")
        out.append(f"    {Table_}_update_async(r, fd, values, id, on_{name}_updated, NULL);")
        out.append("}")
        out.append("")

    # delete
    out.append(f"static int on_{name}_deleted(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {{")
    out.append("    (void)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {")
    out.append('        send_res(r, client_fd, "500 Internal Server Error", "text/plain", "500");')
    out.append("        return 0;")
    out.append("    }")
    out.append("    if (atoi(PQcmdTuples(res)) < 1) {")
    out.append('        send_res(r, client_fd, "404 Not Found", "text/plain", "404");')
    out.append("        return 0;")
    out.append("    }")
    out.append('    send_res(r, client_fd, "200 OK", "text/plain", "ok");')
    out.append("    return 0;")
    out.append("}")
    out.append("")
    out.append(f"void delete_{name}(struct io_uring *r, int fd, const char *m, const char *b) {{")
    out.append("    (void)m; (void)b;")
    out.append('    const char *id = route_param("id");')
    out.append('    if (!id) { send_res(r, fd, "400 Bad Request", "text/plain", "400"); return; }')
    out.append(f"    {Table_}_delete_async(r, fd, id, on_{name}_deleted, NULL);")
    out.append("}")

    return "\n".join(out) + "\n"


# ============================================================
# Generacion de utils/auth/ (autenticacion agnostica de esquema, ver
# find_auth_table): reemplaza a los utils/auth/users.c/.h y
# utils/auth/auth.c escritos a mano, con el mismo mecanismo de marcador/
# --force que el resto de dbfiller. Los nombres de funcion
# (Auth_find_by_identity_async, Auth_create_async, auth_model_register)
# son fijos, NO derivados del nombre de tabla -- utils/auth/auth.c no
# necesita saber que tabla los genero.
# ============================================================

def _cq(name: str) -> str:
    """Nombre entre comillas dobles escapadas para incrustar en un
    string C (la sentencia SQL que ese string arma se manda tal cual a
    Postgres) -- necesario porque el nombre de tabla/columna puede ser
    una palabra reservada (ej. "user") o tener mayusculas."""
    return f'\\"{name}\\"'


def _varchar_len(data_type: str):
    m = re.match(r"(?:character\s+varying|varchar|character|char)\s*\(\s*(\d+)\s*\)", data_type.strip(), re.IGNORECASE)
    return int(m.group(1)) if m else None


def auth_extra_columns(auth: "AuthCandidate") -> list:
    """Columnas insertables de la tabla de auth, sin contar la de
    identidad ni la de password (esas se manejan aparte, ver
    gen_auth_c) -- mismo classify() que usa el CRUD generico."""
    _all, insertable, _updatable, _skipped = classify(auth.table)
    return [c for c in insertable if c.name != auth.identity_col.name]


def gen_auth_model_h(auth: "AuthCandidate", extra_cols: list) -> str:
    table = auth.table
    pk = table.columns[table.pk_index].name
    out = [MARKER, ""]
    extra_doc = "".join(f", [{i + 2}] {c.name}" for i, c in enumerate(extra_cols))
    out.append(
        "/*\n"
        " * utils/auth/auth_model.h - SQL y prepared statements de autenticacion\n"
        " *\n"
        f" * Generado a partir de la tabla '{table.name}' (columna de identidad\n"
        f" * '{auth.identity_col.name}', columna de password '{auth.password_col.name}',\n"
        f" * PK '{pk}') -- ver tools/dbfiller/README.md, seccion \"Autenticacion\".\n"
        " * Los nombres de funcion son fijos (no dependen del nombre de tabla):\n"
        " * utils/auth/auth.c los llama siempre igual, sin importar que esquema\n"
        " * los genero.\n"
        " */"
    )
    out.append("#ifndef UTILS_AUTH_AUTH_MODEL_H")
    out.append("#define UTILS_AUTH_AUTH_MODEL_H")
    out.append("")
    out.append("#include <liburing.h>")
    out.append('#include "../../config/db.h"')
    out.append("")
    out.append("void auth_model_register(void);")
    out.append("")
    out.append(
        "/*\n"
        " * Auth_find_by_identity_async - busca por la columna de identidad.\n"
        " * Si PQntuples(res) == 1: columna 0 = id (texto), columna 1 =\n"
        " * password ya hasheado (texto). Cero filas = no existe.\n"
        " */"
    )
    out.append("void Auth_find_by_identity_async(struct io_uring *r, int client_fd, const char *identity, cb callback, void *userdata);")
    out.append("")
    out.append("/* Cantidad exacta de elementos que debe tener 'values' en Auth_create_async. */")
    out.append(f"#define AUTH_CREATE_PARAMS {2 + len(extra_cols)}")
    out.append(
        "/*\n"
        " * Auth_create_async - crea un registro nuevo. 'values' trae, en este\n"
        f" * orden: [0] identidad, [1] password ya hasheado (ver\n"
        f" * utils/auth/password.h){extra_doc}.\n"
        " * Resultado (si la insercion tuvo exito): columna 0 = id.\n"
        " */"
    )
    out.append("void Auth_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata);")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def gen_auth_model_c(auth: "AuthCandidate", extra_cols: list) -> str:
    table = auth.table
    pk = table.columns[table.pk_index].name
    ident = auth.identity_col.name
    pwd = auth.password_col.name
    qtable = _cq(table.name)
    insert_cols = ", ".join([_cq(ident), _cq(pwd)] + [_cq(c.name) for c in extra_cols])
    placeholders = ", ".join(f"${i + 1}" for i in range(2 + len(extra_cols)))

    out = [MARKER, '#include "auth_model.h"', ""]
    out.append('#define AUTH_STMT_FIND_BY_IDENTITY "auth_find_by_identity"')
    out.append('#define AUTH_STMT_CREATE           "auth_create"')
    out.append("")
    out.append("void auth_model_register(void) {")
    out.append(
        "    db_register_prepared(AUTH_STMT_FIND_BY_IDENTITY,\n"
        f'        "SELECT {_cq(pk)}, {_cq(pwd)} FROM {qtable} WHERE {_cq(ident)} = $1;");\n'
    )
    out.append(
        "    db_register_prepared(AUTH_STMT_CREATE,\n"
        f'        "INSERT INTO {qtable} ({insert_cols}) VALUES ({placeholders}) RETURNING {_cq(pk)};");\n'
    )
    out.append("}")
    out.append("")
    out.append("void Auth_find_by_identity_async(struct io_uring *r, int client_fd, const char *identity, cb callback, void *userdata) {")
    out.append("    const char *params[1] = { identity };")
    out.append("    db_query_prepared_params_async(r, client_fd, AUTH_STMT_FIND_BY_IDENTITY, 1, params, 0, callback, userdata);")
    out.append("}")
    out.append("")
    out.append("void Auth_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata) {")
    out.append("    db_query_prepared_params_async(r, client_fd, AUTH_STMT_CREATE, AUTH_CREATE_PARAMS, values, 0, callback, userdata);")
    out.append("}")
    return "\n".join(out) + "\n"


def gen_auth_c(auth: "AuthCandidate", extra_cols: list, identity_buf: int) -> str:
    ident = auth.identity_col.name
    extra_names = ", ".join(c.name for c in extra_cols) if extra_cols else "(ninguna)"
    out = [MARKER, ""]
    out.append(
        "/*\n"
        " * utils/auth/auth.c - implementacion de register_user/login_user/me\n"
        " * (ver utils/auth/auth.h)\n"
        " *\n"
        f" * Generado a partir de la tabla '{auth.table.name}' (columna de\n"
        f" * identidad '{ident}', ver tools/dbfiller/README.md, seccion\n"
        " * \"Autenticacion\"). El body de POST /api/auth/register siempre usa\n"
        " * las claves \"username\"/\"password\" (nombres fijos del framework,\n"
        " * no derivados del esquema -- el valor real detras de \"username\"\n"
        f" * puede ser un email), mas las demas columnas requeridas por esa\n"
        f" * tabla: {extra_names}.\n"
        " */"
    )
    out.append('#include "auth.h"')
    out.append("")
    out.append("#include <stdio.h>")
    out.append("#include <stdlib.h>")
    out.append("#include <string.h>")
    out.append("")
    out.append('#include "../../config/db.h"')
    out.append('#include "auth_model.h"')
    out.append('#include "../http/router.h"')
    out.append('#include "../http/http.h"')
    out.append('#include "../http/json_body.h"')
    out.append('#include "../net/conn_limit.h"')
    out.append('#include "password.h"')
    out.append('#include "jwt.h"')
    out.append('#include "login_limit.h"')
    out.append("")
    out.append(f"#define AUTH_IDENTITY_BUF {identity_buf}")
    out.append("")
    out.append(
        "/*\n"
        " * identity_is_valid - chequeo generico sobre el identificador de\n"
        " * login (puede ser un email, un username, etc. segun el esquema --\n"
        " * ver auth_model.c): largo razonable y sin comillas/backslash/\n"
        " * caracteres de control, lo minimo para poder incrustarlo sin\n"
        " * escapar en el payload JSON armado a mano de jwt_create\n"
        " * (utils/auth/jwt.c).\n"
        " */"
    )
    out.append("static int identity_is_valid(const char *u, size_t len) {")
    out.append("    if (len < 1 || len >= AUTH_IDENTITY_BUF) return 0;")
    out.append("    for (size_t i = 0; i < len; i++) {")
    out.append("        unsigned char c = (unsigned char)u[i];")
    out.append('        if (c == \'"\' || c == \'\\\\\' || c < 0x20 || c == 0x7f) return 0;')
    out.append("    }")
    out.append("    return 1;")
    out.append("}")
    out.append("")
    out.append(
        "/*\n"
        " * parse_credentials - extrae {\"username\":\"...\",\"password\":\"...\"}\n"
        " * del body JSON de un request (login: no acepta columnas extra, a\n"
        " * diferencia de register_user).\n"
        " */"
    )
    out.append("static int parse_credentials(const char *buf, char *identity_out, size_t identity_sz, char *password_out, size_t password_sz) {")
    out.append("    jsmntok_t tokens[16];")
    out.append("    const char *body = NULL;")
    out.append("    int ntok = http_parse_json_body(buf, tokens, sizeof(tokens) / sizeof(tokens[0]), &body);")
    out.append("    if (ntok < 1 || !body) return 0;")
    out.append('    int st_identity = json_body_field(body, tokens, ntok, "username", identity_out, identity_sz);')
    out.append('    int st_password = json_body_field(body, tokens, ntok, "password", password_out, password_sz);')
    out.append("    return st_identity == 1 && st_password == 1;")
    out.append("}")
    out.append("")
    out.append(
        "/*\n"
        " * issue_token_response - arma un JWT y lo manda como {\"token\":\"...\"}\n"
        " * Punto unico compartido por register_user y login_user.\n"
        " */"
    )
    out.append("static void issue_token_response(struct io_uring *r, int fd, const char *id_str, const char *identity) {")
    out.append('    const char *secret = getenv("JWT_SECRET");')
    out.append("    char token[JWT_TOKEN_BUF_SIZE];")
    out.append("    if (!secret || !jwt_create(secret, id_str, identity, g_jwt_expires_seconds, token, sizeof(token))) {")
    out.append('        send_res(r, fd, "500 Internal Server Error", "text/plain", "500");')
    out.append("        return;")
    out.append("    }")
    out.append("")
    out.append("    char body[JWT_TOKEN_BUF_SIZE + 64];")
    out.append('    snprintf(body, sizeof(body), "{\\"token\\":\\"%s\\"}", token);')
    out.append("    res_json(r, fd, body);")
    out.append("}")
    out.append("")

    out.append("typedef struct {")
    out.append("    char identity[AUTH_IDENTITY_BUF];")
    out.append("} register_ctx_t;")
    out.append("")
    out.append("static int on_auth_created(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {")
    out.append("    register_ctx_t *ctx = (register_ctx_t *)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
    out.append('        send_res(r, client_fd, "409 Conflict", "text/plain", "409");')
    out.append("        free(ctx);")
    out.append("        return 0;")
    out.append("    }")
    out.append("    issue_token_response(r, client_fd, PQgetvalue(res, 0, 0), ctx->identity);")
    out.append("    free(ctx);")
    out.append("    return 0;")
    out.append("}")
    out.append("")

    n_tokens = (2 + len(extra_cols)) * 2 + 4
    out.append("void register_user(struct io_uring *r, int f, const char *m, const char *b) {")
    out.append("    (void)m;")
    out.append("")
    out.append(f"    jsmntok_t tokens[{n_tokens}];")
    out.append("    const char *json_body = NULL;")
    out.append("    int ntok = http_parse_json_body(b, tokens, sizeof(tokens) / sizeof(tokens[0]), &json_body);")
    out.append('    if (ntok < 1 || !json_body) { send_res(r, f, "400 Bad Request", "text/plain", "400"); return; }')
    out.append("")
    out.append("    char identity[AUTH_IDENTITY_BUF];")
    out.append('    int st_identity = json_body_field(json_body, tokens, ntok, "username", identity, sizeof(identity));')
    out.append("    char password[128];")
    out.append('    int st_password = json_body_field(json_body, tokens, ntok, "password", password, sizeof(password));')
    for c in extra_cols:
        out.append(f"    char buf_{c.name}[256];")
        out.append(f'    int st_{c.name} = json_body_field(json_body, tokens, ntok, "{c.name}", buf_{c.name}, sizeof(buf_{c.name}));')
    out.append("")
    cond = ["st_identity != 1", "!identity_is_valid(identity, strlen(identity))", "st_password != 1", "strlen(password) < 8"]
    for c in extra_cols:
        cond.append(f"st_{c.name} < 0" if c.nullable else f"st_{c.name} != 1")
    out.append("    if (" + " ||\n        ".join(cond) + ") {")
    out.append("        explicit_bzero(password, sizeof(password));")
    out.append('        send_res(r, f, "400 Bad Request", "text/plain", "400");')
    out.append("        return;")
    out.append("    }")
    out.append("")
    out.append("    char hash[PASSWORD_HASH_BUF_SIZE];")
    out.append("    int hashed = password_hash(password, hash, sizeof(hash));")
    out.append("    explicit_bzero(password, sizeof(password));")
    out.append("    if (!hashed) {")
    out.append('        send_res(r, f, "500 Internal Server Error", "text/plain", "500");')
    out.append("        return;")
    out.append("    }")
    out.append("")
    out.append("    register_ctx_t *ctx = calloc(1, sizeof(register_ctx_t));")
    out.append("    if (!ctx) {")
    out.append('        send_res(r, f, "500 Internal Server Error", "text/plain", "500");')
    out.append("        return;")
    out.append("    }")
    out.append('    snprintf(ctx->identity, sizeof(ctx->identity), "%s", identity);')
    out.append("")
    out.append("    const char *values[AUTH_CREATE_PARAMS];")
    out.append("    values[0] = identity;")
    out.append("    values[1] = hash;")
    for i, c in enumerate(extra_cols):
        out.append(f"    values[{2 + i}] = (st_{c.name} == 1) ? buf_{c.name} : NULL;")
    out.append("")
    out.append("    Auth_create_async(r, f, values, on_auth_created, ctx);")
    out.append("}")
    out.append("")

    out.append("typedef struct {")
    out.append("    char identity[AUTH_IDENTITY_BUF];")
    out.append("    char password[128];")
    out.append("    uint32_t ip;")
    out.append("} login_ctx_t;")
    out.append("")
    out.append("static void free_login_ctx(login_ctx_t *ctx) {")
    out.append("    explicit_bzero(ctx->password, sizeof(ctx->password));")
    out.append("    free(ctx);")
    out.append("}")
    out.append("")
    out.append("static int on_auth_found_for_login(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {")
    out.append("    login_ctx_t *ctx = (login_ctx_t *)userdata;")
    out.append("    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {")
    out.append("        login_limit_record_failure(ctx->ip);")
    out.append('        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");')
    out.append("        free_login_ctx(ctx);")
    out.append("        return 0;")
    out.append("    }")
    out.append("")
    out.append("    const char *id_str = PQgetvalue(res, 0, 0);")
    out.append("    const char *stored_hash = PQgetvalue(res, 0, 1);")
    out.append("")
    out.append("    if (!password_verify(ctx->password, stored_hash)) {")
    out.append("        login_limit_record_failure(ctx->ip);")
    out.append('        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");')
    out.append("        free_login_ctx(ctx);")
    out.append("        return 0;")
    out.append("    }")
    out.append("")
    out.append("    login_limit_record_success(ctx->ip);")
    out.append("    issue_token_response(r, client_fd, id_str, ctx->identity);")
    out.append("    free_login_ctx(ctx);")
    out.append("    return 0;")
    out.append("}")
    out.append("")
    out.append("void login_user(struct io_uring *r, int f, const char *m, const char *b) {")
    out.append("    (void)m;")
    out.append("")
    out.append("    uint32_t ip = conn_limit_get_ip(f);")
    out.append("    if (g_trust_proxy_headers) {")
    out.append("        uint32_t forwarded = extract_forwarded_ip(b);")
    out.append("        if (forwarded) ip = forwarded;")
    out.append("    }")
    out.append("    if (!login_limit_allowed(ip)) {")
    out.append('        send_res(r, f, "429 Too Many Requests", "text/plain", "429");')
    out.append("        return;")
    out.append("    }")
    out.append("")
    out.append("    login_ctx_t *ctx = calloc(1, sizeof(login_ctx_t));")
    out.append("    if (!ctx) {")
    out.append('        send_res(r, f, "500 Internal Server Error", "text/plain", "500");')
    out.append("        return;")
    out.append("    }")
    out.append("    ctx->ip = ip;")
    out.append("")
    out.append("    if (!parse_credentials(b, ctx->identity, sizeof(ctx->identity), ctx->password, sizeof(ctx->password))) {")
    out.append("        free_login_ctx(ctx);")
    out.append('        send_res(r, f, "400 Bad Request", "text/plain", "400");')
    out.append("        return;")
    out.append("    }")
    out.append("")
    out.append("    Auth_find_by_identity_async(r, f, ctx->identity, on_auth_found_for_login, ctx);")
    out.append("}")
    out.append("")

    out.append("void me(struct io_uring *r, int f, const char *m, const char *b) {")
    out.append("    (void)m;")
    out.append("    (void)b;")
    out.append("")
    out.append("    char body[320];")
    out.append('    snprintf(body, sizeof(body), "{\\"sub\\":\\"%s\\",\\"username\\":\\"%s\\"}", jwt_claim("sub"), jwt_claim("username"));')
    out.append("    res_json(r, f, body);")
    out.append("}")

    return "\n".join(out) + "\n"


def codegen_write_auth(auth: "AuthCandidate", repo_root: str, force: bool):
    extra_cols = auth_extra_columns(auth)
    identity_buf = (_varchar_len(auth.identity_col.data_type) or 128) + 1

    model_h = gen_auth_model_h(auth, extra_cols)
    model_c = gen_auth_model_c(auth, extra_cols)
    auth_c = gen_auth_c(auth, extra_cols, identity_buf)

    auth_dir = os.path.join(repo_root, SRC_DIR, "utils", "auth")
    _write_file(os.path.join(auth_dir, "auth_model.h"), model_h, force)
    _write_file(os.path.join(auth_dir, "auth_model.c"), model_c, force)
    _write_file(os.path.join(auth_dir, "auth.c"), auth_c, force)


# ---- escritura a disco, con proteccion de archivos escritos a mano Y de
# archivos editados a mano DESPUES de haber sido generados ----
#
# El MARKER de la primera linea por si solo NO alcanza para detectar la
# segunda situacion: identifica "esto lo genero dbfiller alguna vez", no
# "esto sigue igual a como lo genero dbfiller la ultima vez". Sin mas
# informacion, cualquier archivo que ya tenga el marcador se
# sobreescribiria entero en cada corrida, silenciosamente, aunque se le
# hubiera agregado logica de negocio a mano despues -- exactamente lo que
# se supone que el marcador evita. Por eso cada archivo generado lleva
# una SEGUNDA linea con el hash sha256 del contenido que le sigue: al
# regenerar, si el hash guardado ya no coincide con el contenido actual,
# alguien lo toco despues de la ultima generacion y hace falta --force
# (mismo mensaje/semantica que un archivo escrito a mano desde el
# principio, ver _file_generation_state).
_HASH_LINE_RE = re.compile(r"^/\* dbfiller:sha256:([0-9a-f]{64}) \*/$")


def _content_hash(body: str) -> str:
    return hashlib.sha256(body.encode("utf-8")).hexdigest()


def _split_generated(content: str):
    """Si 'content' tiene el formato que escribe _write_file (MARKER +
    linea de hash + resto), retorna (hash_declarado, resto). Si el
    archivo no tiene ese formato exacto (nunca paso por dbfiller, o es
    de una version de dbfiller anterior a este chequeo de hash), retorna
    (None, content) -- se trata igual que un archivo escrito a mano."""
    if not content.startswith(MARKER + "\n"):
        return None, content
    rest = content[len(MARKER) + 1:]
    first_nl = rest.find("\n")
    if first_nl == -1:
        return None, content
    hash_line, body = rest[:first_nl], rest[first_nl + 1:]
    m = _HASH_LINE_RE.match(hash_line)
    if not m:
        return None, content
    return m.group(1), body


def _file_generation_state(path: str) -> str:
    """Uno de:
    'absent'  - no existe, se puede crear sin mas.
    'foreign' - existe pero no tiene el formato de dbfiller (escrito a
                mano, o generado por una version vieja sin hash).
    'clean'   - generado por dbfiller y sin tocar desde la ultima vez.
    'dirty'   - generado por dbfiller, pero el contenido ya no coincide
                con el hash guardado -- se edito a mano despues.
    Solo 'clean' se sobreescribe sin pedir --force."""
    if not os.path.exists(path):
        return "absent"
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    declared_hash, body = _split_generated(content)
    if declared_hash is None:
        return "foreign"
    return "clean" if _content_hash(body) == declared_hash else "dirty"


def _write_file(path: str, content: str, force: bool):
    if not content.startswith(MARKER + "\n"):
        raise GenerateError(f"'{path}': contenido generado sin el marcador esperado (bug interno de dbfiller)")

    state = _file_generation_state(path)
    if state in ("foreign", "dirty") and not force:
        reason = (
            "ya existe y no fue generado por dbfiller (falta el marcador)"
            if state == "foreign"
            else "fue generado por dbfiller pero se edito a mano despues de esa generacion (el hash ya no coincide)"
        )
        raise GenerateError(f"'{path}' {reason} -- usa --force para sobreescribir")

    body = content[len(MARKER) + 1:]
    final_content = f"{MARKER}\n/* dbfiller:sha256:{_content_hash(body)} */\n{body}"

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(final_content)


def codegen_write_table(table: Table, repo_root: str, force: bool):
    cl = classify(table)
    has_update = len(cl[2]) > 0  # updatable
    TABLE = table.name.upper()
    Table_ = to_pascal(table.name)

    model_h = gen_model_h(table, TABLE, Table_, has_update)
    model_c = gen_model_c(table, cl, TABLE, Table_, has_update)
    controller_h = gen_controller_h(table, TABLE, has_update)
    controller_c = gen_controller_c(table, cl, Table_, has_update)

    _write_file(os.path.join(repo_root, SRC_DIR, "models", f"{table.name}.h"), model_h, force)
    _write_file(os.path.join(repo_root, SRC_DIR, "models", f"{table.name}.c"), model_c, force)
    _write_file(os.path.join(repo_root, SRC_DIR, "controllers", f"{table.name}.h"), controller_h, force)
    _write_file(os.path.join(repo_root, SRC_DIR, "controllers", f"{table.name}.c"), controller_c, force)

    return has_update


# ============================================================
# Patch de routes/index.h y models/registry.h (puerto de repo_patch.c)
# ============================================================

def _line_start(content: str, pos: int) -> int:
    return content.rfind("\n", 0, pos) + 1


def _line_end(content: str, pos: int) -> int:
    i = content.find("\n", pos)
    return i + 1 if i != -1 else len(content)


_MARKER_STYLES = {
    # comment_style -> (begin, end, anchor), parametrizados por %(table)s / %(tag)s
    "c": ("/* dbfiller:%(table)s:%(tag)s:begin */", "/* dbfiller:%(table)s:%(tag)s:end */", "/* dbfiller:%(tag)s-point */"),
    "yaml": ("# dbfiller:%(table)s:%(tag)s:begin", "# dbfiller:%(table)s:%(tag)s:end", "# dbfiller:%(tag)s-point"),
}


def _upsert_block(content: str, table: str, tag: str, comment_style: str, new_body: str) -> str:
    begin_tpl, end_tpl, anchor_tpl = _MARKER_STYLES[comment_style]
    begin_marker = begin_tpl % {"table": table, "tag": tag}
    end_marker = end_tpl % {"table": table, "tag": tag}
    anchor = anchor_tpl % {"tag": tag}

    begin_pos = content.find(begin_marker)
    if begin_pos != -1:
        end_pos = content.find(end_marker, begin_pos)
        if end_pos == -1:
            raise GenerateError(f"bloque '{begin_marker}' sin marcador de cierre correspondiente")
        seg1_end = _line_end(content, begin_pos)
        seg2_start = _line_start(content, end_pos)
        return content[:seg1_end] + new_body + content[seg2_start:]

    anchor_pos = content.find(anchor)
    if anchor_pos == -1:
        raise GenerateError(f"no se encontro el marcador '{anchor}' -- agregalo una vez a mano (ver README)")
    anchor_line = _line_start(content, anchor_pos)
    return content[:anchor_line] + begin_marker + "\n" + new_body + end_marker + "\n" + content[anchor_line:]


def _patch_block(path: str, table: str, tag: str, comment_style: str, body: str):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    content = _upsert_block(content, table, tag, comment_style, body)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def repo_patch_apply(repo_root: str, table_name: str, has_update: bool):
    routes_path = os.path.join(repo_root, SRC_DIR, "routes/index.h")
    include_line = f'#include "../controllers/{table_name}.h"\n'
    routes_body = f'    get("/api/{table_name}", list_{table_name});\n'
    routes_body += f'    get("/api/{table_name}/:id", get_{table_name});\n'
    routes_body += f'    post_auth("/api/{table_name}", create_{table_name});\n'
    if has_update:
        routes_body += f'    put_auth("/api/{table_name}/:id", update_{table_name});\n'
    routes_body += f'    del_auth("/api/{table_name}/:id", delete_{table_name});\n'
    _patch_block(routes_path, table_name, "includes", "c", include_line)
    _patch_block(routes_path, table_name, "routes", "c", routes_body)

    registry_path = os.path.join(repo_root, SRC_DIR, "models/registry.h")
    include_line = f'#include "../models/{table_name}.h"\n'
    models_body = f"    {table_name}_register();\n"
    _patch_block(registry_path, table_name, "includes", "c", include_line)
    _patch_block(registry_path, table_name, "models", "c", models_body)


# ============================================================
# Patch de docs-ui/openapi.yaml (Swagger UI, servido en /docs)
# ============================================================

def _openapi_type_for(data_type: str) -> str:
    """Fragmento YAML inline para el tipo de una columna -- mapeo
    best-effort sobre el mismo subconjunto de tipos que ya reconoce
    _extract_type (ver README, "Limitaciones conocidas"), no exhaustivo."""
    t = data_type.lower().split("(")[0].strip()
    if t in ("integer", "int", "int4", "smallint", "int2", "bigint", "int8", "serial", "bigserial", "smallserial"):
        return "{ type: integer }"
    if t in ("numeric", "decimal", "real", "double precision", "float4", "float8", "money"):
        return "{ type: number }"
    if t in ("boolean", "bool"):
        return "{ type: boolean }"
    if t == "date":
        return "{ type: string, format: date }"
    if t.startswith("timestamp") or t.startswith("time"):
        return "{ type: string, format: date-time }"
    if t == "uuid":
        return "{ type: string, format: uuid }"
    if t in ("json", "jsonb"):
        return "{ type: object }"
    return "{ type: string }"


def gen_openapi_schemas(table: Table, cl) -> str:
    """<Tabla> (todas las columnas publicas), <Tabla>Create (insertable)
    y <Tabla>Update (updatable, si hay) -- mismo criterio de columnas que
    ya usa el codegen C (classify(), sensibles excluidas)."""
    all_cols, insertable, updatable, _skipped = cl
    Table_ = to_pascal(table.name)
    out = [f"    {Table_}:", "      type: object", "      properties:"]
    for c in all_cols:
        out.append(f"        {c.name}: {_openapi_type_for(c.data_type)}")

    if insertable:
        out.append(f"    {Table_}Create:")
        out.append("      type: object")
        required = [c.name for c in insertable if not c.nullable]
        if required:
            out.append(f"      required: [{', '.join(required)}]")
        out.append("      properties:")
        for c in insertable:
            out.append(f"        {c.name}: {_openapi_type_for(c.data_type)}")

    if updatable:
        out.append(f"    {Table_}Update:")
        out.append("      type: object")
        out.append("      properties:")
        for c in updatable:
            out.append(f"        {c.name}: {_openapi_type_for(c.data_type)}")

    return "\n".join(out) + "\n"


def gen_openapi_paths(table: Table, cl, has_update: bool) -> str:
    all_cols, insertable, updatable, _skipped = cl
    name = table.name
    Table_ = to_pascal(name)
    out = []

    out.append(f"  /api/{name}:")
    out.append("    get:")
    out.append(f"      tags: [{name}]")
    out.append(f"      summary: Listar {name}")
    out.append("      responses:")
    out.append('        "200":')
    out.append(f"          description: Lista de {name}")
    out.append("          content:")
    out.append("            application/json:")
    out.append("              schema:")
    out.append("                type: object")
    out.append("                properties:")
    out.append("                  data:")
    out.append("                    type: array")
    out.append("                    items:")
    out.append(f"                      $ref: '#/components/schemas/{Table_}'")
    out.append("    post:")
    out.append(f"      tags: [{name}]")
    out.append(f"      summary: Crear {name}")
    out.append("      security:")
    out.append("        - bearerAuth: []")
    if insertable:
        out.append("      requestBody:")
        out.append("        required: true")
        out.append("        content:")
        out.append("          application/json:")
        out.append("            schema:")
        out.append(f"              $ref: '#/components/schemas/{Table_}Create'")
    out.append("      responses:")
    out.append('        "201":')
    out.append("          description: Creado")
    out.append("          content:")
    out.append("            application/json:")
    out.append("              schema:")
    out.append("                type: object")
    out.append("                properties:")
    out.append("                  data:")
    out.append(f"                    $ref: '#/components/schemas/{Table_}'")
    out.append('        "400":')
    out.append("          description: Body inválido")
    out.append('        "401":')
    out.append("          description: Token faltante, inválido o vencido")
    out.append('        "409":')
    out.append("          description: Violación de constraint (FK, UNIQUE, etc.)")

    out.append(f"  /api/{name}/{{id}}:")
    out.append("    get:")
    out.append(f"      tags: [{name}]")
    out.append(f"      summary: Buscar {name} por id")
    out.append("      parameters:")
    out.append("        - name: id")
    out.append("          in: path")
    out.append("          required: true")
    out.append("          schema: { type: string }")
    out.append("      responses:")
    out.append('        "200":')
    out.append("          description: OK")
    out.append("          content:")
    out.append("            application/json:")
    out.append("              schema:")
    out.append("                type: object")
    out.append("                properties:")
    out.append("                  data:")
    out.append(f"                    $ref: '#/components/schemas/{Table_}'")
    out.append('        "404":')
    out.append("          description: No existe")

    if has_update:
        out.append("    put:")
        out.append(f"      tags: [{name}]")
        out.append(f"      summary: Actualizar {name}")
        out.append("      security:")
        out.append("        - bearerAuth: []")
        out.append("      parameters:")
        out.append("        - name: id")
        out.append("          in: path")
        out.append("          required: true")
        out.append("          schema: { type: string }")
        if updatable:
            out.append("      requestBody:")
            out.append("        required: true")
            out.append("        content:")
            out.append("          application/json:")
            out.append("            schema:")
            out.append(f"              $ref: '#/components/schemas/{Table_}Update'")
        out.append("      responses:")
        out.append('        "200":')
        out.append("          description: Actualizado")
        out.append("          content:")
        out.append("            application/json:")
        out.append("              schema:")
        out.append("                type: object")
        out.append("                properties:")
        out.append("                  data:")
        out.append(f"                    $ref: '#/components/schemas/{Table_}'")
        out.append('        "400":')
        out.append("          description: Body inválido")
        out.append('        "401":')
        out.append("          description: Token faltante, inválido o vencido")
        out.append('        "404":')
        out.append("          description: No existe")

    out.append("    delete:")
    out.append(f"      tags: [{name}]")
    out.append(f"      summary: Borrar {name}")
    out.append("      security:")
    out.append("        - bearerAuth: []")
    out.append("      parameters:")
    out.append("        - name: id")
    out.append("          in: path")
    out.append("          required: true")
    out.append("          schema: { type: string }")
    out.append("      responses:")
    out.append('        "200":')
    out.append("          description: Borrado")
    out.append('        "401":')
    out.append("          description: Token faltante, inválido o vencido")
    out.append('        "404":')
    out.append("          description: No existe")

    return "\n".join(out) + "\n"


def openapi_patch_apply(repo_root: str, table: Table, cl, has_update: bool):
    """No falla si el proyecto no tiene docs-ui/openapi.yaml (documentar
    con Swagger es un extra del boilerplate, no un requisito para que el
    codigo generado compile/funcione -- a diferencia de routes/index.h y
    models/registry.h, que si son obligatorios)."""
    path = os.path.join(repo_root, "docs-ui", "openapi.yaml")
    if not os.path.isfile(path):
        return
    _patch_block(path, table.name, "schemas", "yaml", gen_openapi_schemas(table, cl))
    _patch_block(path, table.name, "paths", "yaml", gen_openapi_paths(table, cl, has_update))


# ============================================================
# Orquestacion (equivalente a dbfiller_generate_table, generate.c)
# ============================================================

def generate_table(table: Table, repo_root: str, force: bool):
    """Retorna (status, message, has_update) -- status es uno de
    "ok"/"skipped"/"error". "skipped" (tabla sin PK simple) es un caso
    documentado y esperado (ver README), no un error: no debe hacer
    fallar `docker build .` -- a diferencia de "error" (conflicto de
    archivo escrito a mano, marcador faltante, etc), que si debe."""
    if table.pk_index < 0:
        return "skipped", "saltada -- no tiene una PRIMARY KEY de una sola columna (no soportado)", False
    try:
        has_update = codegen_write_table(table, repo_root, force)
        repo_patch_apply(repo_root, table.name, has_update)
        openapi_patch_apply(repo_root, table, classify(table), has_update)
    except GenerateError as e:
        return "error", str(e), False
    return "ok", "", has_update


# ============================================================
# CLI
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="Genera endpoints CRUD para cerver a partir de db/init/*.sql")
    parser.add_argument("--repo-root", default=".", help="raiz del repo cerver (default: directorio actual)")
    parser.add_argument("--schema", nargs="+", default=None,
                         help="archivos/patrones .sql a leer (default: <repo-root>/db/init/*.sql)")
    parser.add_argument("--force", action="store_true",
                         help="sobreescribe archivos existentes aunque no tengan la marca de dbfiller")
    parser.add_argument("--auth-table", default=None,
                         help="fuerza cual tabla usar para generar utils/auth/ (autodetectada por default, "
                              "ver README seccion 'Autenticacion'); 'none' desactiva la deteccion")
    args = parser.parse_args()

    if args.schema:
        paths = []
        for pattern in args.schema:
            matched = sorted(glob.glob(pattern))
            if not matched and os.path.isfile(pattern):
                matched = [pattern]
            paths.extend(matched)
    else:
        paths = sorted(glob.glob(os.path.join(args.repo_root, "db", "init", "*.sql")))

    if not paths:
        print("No se encontro ningun archivo .sql (ver --schema).", file=sys.stderr)
        return 1

    tables = parse_schema_files(paths)
    if not tables:
        print("No se encontro ningun CREATE TABLE en los archivos dados.", file=sys.stderr)
        return 1

    errors = 0

    auth = None
    if args.auth_table != "none":
        try:
            auth = find_auth_table(tables, args.auth_table)
        except GenerateError as e:
            print(f"auth: ERROR: {e}", file=sys.stderr)
            errors += 1

    if auth:
        try:
            codegen_write_auth(auth, args.repo_root, args.force)
            print(f"auth: OK (tabla '{auth.table.name}', utils/auth/auth_model.c/.h + utils/auth/auth.c generados)")
        except GenerateError as e:
            print(f"auth: ERROR: {e}", file=sys.stderr)
            errors += 1
    elif args.auth_table != "none":
        print("auth: ninguna tabla califica (ver README, seccion 'Autenticacion') -- utils/auth/ sin cambios", file=sys.stderr)

    for table in tables:
        if auth and table is auth.table:
            continue
        status, message, has_update = generate_table(table, args.repo_root, args.force)
        if status == "ok":
            suffix = "" if has_update else ", sin update: no tiene columnas actualizables"
            print(f"{table.name}: OK (controllers/{table.name}.c/.h, models/{table.name}.c/.h, rutas registradas{suffix})")
        elif status == "skipped":
            print(f"{table.name}: {message}", file=sys.stderr)
        else:
            print(f"{table.name}: ERROR: {message}", file=sys.stderr)
            errors += 1

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
