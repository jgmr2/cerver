#!/usr/bin/env python3
"""
tests/integration/test_integration.py - test de integración contra el
stack real (backend + Postgres), reemplaza la batería de curl/ab que se
usó a mano durante el desarrollo por algo repetible y apto para CI.

Uso:
    docker compose up -d
    python3 tests/integration/test_integration.py
    # exit code 0 = todo ok, 1 = algo falló (para usar como gate de CI)

Solo librería estándar (http.client, socket, json) — nada que instalar
aparte para correr esto en cualquier lado, incluido el runner de GitHub
Actions sin pasos extra de setup.

No gestiona el ciclo de vida del stack (no hace `docker compose up/down`)
a propósito: eso es responsabilidad de quien lo invoca (ver
.github/workflows/docker-image.yml), así el mismo script sirve para
correrlo a mano contra un stack ya levantado.
"""
import http.client
import json
import os
import random
import socket
import string
import sys
import time

HOST = os.environ.get("TEST_HOST", "localhost")
PORT = int(os.environ.get("TEST_PORT", "8080"))

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    status = "ok  " if cond else "FAIL"
    print(f"{status}: {name}" + (f" ({detail})" if detail and not cond else ""))


def rand_username(prefix):
    return prefix + "_" + "".join(random.choices(string.ascii_lowercase + string.digits, k=10))


def request(method, path, headers=None, body=None, timeout=5):
    conn = http.client.HTTPConnection(HOST, PORT, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, dict(resp.getheaders()), data
    finally:
        conn.close()


def raw_request(raw_bytes, timeout=5, read_timeout=2):
    """Manda bytes crudos por un socket propio — para casos (como
    Content-Length + Transfer-Encoding a la vez) que un cliente HTTP
    normal no te deja construir porque los "arregla" por vos."""
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(raw_bytes)
        s.settimeout(read_timeout)
        chunks = []
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        except socket.timeout:
            pass
        return b"".join(chunks)
    finally:
        s.close()


def wait_for_healthy(retries=30, delay=1.0):
    for _ in range(retries):
        try:
            status, _, _ = request("GET", "/healthz", timeout=2)
            if status == 200:
                return True
        except (ConnectionRefusedError, OSError, http.client.HTTPException):
            pass
        time.sleep(delay)
    return False


# ---------------------------------------------------------------------
# grupos de test
# ---------------------------------------------------------------------

def test_health_and_basic():
    status, _, body = request("GET", "/healthz")
    check("GET /healthz -> 200 ok", status == 200 and body == b"ok", f"status={status} body={body!r}")

    status, _, body = request("GET", "/api")
    try:
        parsed = json.loads(body)
        ok = status == 200 and "bytes" in parsed and "hex" in parsed
    except ValueError:
        ok = False
    check("GET /api -> 200 con {bytes,hex}", ok, f"status={status} body={body!r}")

    status, _, body = request("GET", "/api/echo/hola")
    check("GET /api/echo/:msg -> 200 con el msg", status == 200 and body == b'{"msg":"hola"}',
          f"status={status} body={body!r}")

    status, _, body = request("GET", '/api/echo/comillas"y\\backslash')
    try:
        parsed = json.loads(body)
        ok = status == 200 and '"' in parsed.get("msg", "") and "\\" in parsed.get("msg", "")
    except ValueError:
        ok = False
    check("GET /api/echo/:msg escapa comillas/backslash sin romper el JSON", ok, f"body={body!r}")


def test_404_never_spa_for_api():
    status, _, _ = request("GET", "/api/esto-no-existe")
    check("GET /api/<inexistente> -> 404 (nunca fallback SPA)", status == 404, f"status={status}")


def test_static_docs():
    status, headers, body = request("GET", "/docs")
    ok = status == 200 and b"swagger-ui" in body.lower().replace(b" ", b"")
    check("GET /docs sirve Swagger UI real", ok, f"status={status} len={len(body)}")


def test_auth_flow():
    user = rand_username("qa")
    password = "claveSegura123"

    status, _, body = request("POST", "/api/auth/register",
                               headers={"Content-Type": "application/json"},
                               body=json.dumps({"username": user, "password": password}))
    try:
        token = json.loads(body).get("token")
    except ValueError:
        token = None
    check("POST /api/auth/register -> 200 con token", status == 200 and bool(token),
          f"status={status} body={body!r}")

    status, _, _ = request("POST", "/api/auth/register",
                            headers={"Content-Type": "application/json"},
                            body=json.dumps({"username": user, "password": password}))
    check("registro duplicado -> 409", status == 409, f"status={status}")

    status, _, _ = request("POST", "/api/auth/register",
                            headers={"Content-Type": "application/json"},
                            body=json.dumps({"username": user + "2", "password": "corta"}))
    check("password corta -> 400", status == 400, f"status={status}")

    status, _, _ = request("POST", "/api/auth/register",
                            headers={"Content-Type": "application/json"},
                            body=json.dumps({"username": "usuario con espacios!", "password": password}))
    check("username con caracteres invalidos -> 400", status == 400, f"status={status}")

    status, _, body = request("POST", "/api/auth/login",
                               headers={"Content-Type": "application/json"},
                               body=json.dumps({"username": user, "password": password}))
    try:
        login_token = json.loads(body).get("token")
    except ValueError:
        login_token = None
    check("login correcto -> 200 con token", status == 200 and bool(login_token), f"status={status}")

    status_wrong, _, body_wrong = request("POST", "/api/auth/login",
                                           headers={"Content-Type": "application/json"},
                                           body=json.dumps({"username": user, "password": "incorrecta"}))
    status_missing, _, body_missing = request("POST", "/api/auth/login",
                                               headers={"Content-Type": "application/json"},
                                               body=json.dumps({"username": rand_username("noexiste"), "password": password}))
    check("password incorrecta -> 401", status_wrong == 401, f"status={status_wrong}")
    check("usuario inexistente -> 401 (misma respuesta que password incorrecta, no revela existencia)",
          status_missing == 401 and body_missing == body_wrong,
          f"status={status_missing} body={body_missing!r} vs {body_wrong!r}")

    return user, password, login_token


def test_protected_routes(token):
    status, _, body = request("GET", "/api/me", headers={"Authorization": f"Bearer {token}"})
    try:
        sub = json.loads(body).get("sub")
    except ValueError:
        sub = None
    check("GET /api/me con token valido -> 200", status == 200 and sub is not None, f"status={status}")

    status, _, _ = request("GET", "/api/me")
    check("GET /api/me sin token -> 401", status == 401, f"status={status}")

    status, _, _ = request("GET", "/api/me", headers={"Authorization": "Bearer token.invalido.aca"})
    check("GET /api/me con token adulterado -> 401", status == 401, f"status={status}")

    return sub


def test_idor_guardrail(token_a, sub_a):
    user_b = rand_username("qaidor")
    password = "claveSegura123"
    status, _, body = request("POST", "/api/auth/register",
                               headers={"Content-Type": "application/json"},
                               body=json.dumps({"username": user_b, "password": password}))
    token_b = json.loads(body).get("token")

    status, _, _ = request("GET", f"/api/me/{sub_a}", headers={"Authorization": f"Bearer {token_a}"})
    check("GET /api/me/:id con el propio id -> 200", status == 200, f"status={status}")

    status, _, _ = request("GET", f"/api/me/999999999", headers={"Authorization": f"Bearer {token_a}"})
    check("GET /api/me/:id con id ajeno -> 403 (guardrail IDOR)", status == 403, f"status={status}")

    status, _, _ = request("GET", f"/api/me/{sub_a}")
    check("GET /api/me/:id sin token -> 401", status == 401, f"status={status}")


def test_oversized_body_413():
    body = json.dumps({"username": rand_username("qabig"), "password": "x" * 6000})
    status, _, _ = request("POST", "/api/auth/register",
                            headers={"Content-Type": "application/json"}, body=body)
    check("body mas grande que el buffer inicial -> 413 explicito (no corrompe/trunca)",
          status == 413, f"status={status} len(body)={len(body)}")


def test_smuggling_rejected():
    req = (
        b"POST /api/auth/login HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Length: 10\r\n"
        b"Transfer-Encoding: chunked\r\n"
        b"\r\n"
        b"0\r\n\r\n"
    )
    resp = raw_request(req)
    check("Content-Length + Transfer-Encoding juntos -> 400 (anti request-smuggling)",
          b"400" in resp.split(b"\r\n", 1)[0] if resp else False,
          f"resp={resp[:80]!r}")


def test_login_rate_limit():
    """Se corre al final a propósito: agota el cupo de intentos fallidos
    de ESTA ip, y no queremos que le pegue a los tests de arriba.

    El límite es por IP, no por usuario (ver utils/auth/login_limit.h) —
    así que los intentos fallidos de test_auth_flow() de más arriba
    (misma IP: este proceso de test) YA suman al mismo cupo. No asumimos
    un número exacto de intentos hasta el bloqueo por eso: reintentamos
    hasta ver un 429 (con un techo generoso) en vez de contar N a ciegas.
    """
    user = rand_username("qarate")
    password = "claveSegura123"
    request("POST", "/api/auth/register", headers={"Content-Type": "application/json"},
            body=json.dumps({"username": user, "password": password}))

    max_attempts = int(os.environ.get("LOGIN_MAX_ATTEMPTS", "10"))
    hit_429 = False
    for _ in range(max_attempts + 5):  # margen por los fallos ya acumulados de tests anteriores
        status, _, _ = request("POST", "/api/auth/login",
                                headers={"Content-Type": "application/json"},
                                body=json.dumps({"username": user, "password": "mal"}))
        if status == 429:
            hit_429 = True
            break
        if status != 401:
            check("intentos fallidos dan 401 hasta el bloqueo", False, f"status inesperado={status}")
            return
    check(f"tras varios intentos fallidos, la IP queda bloqueada con 429", hit_429)

    status, _, _ = request("POST", "/api/auth/login",
                            headers={"Content-Type": "application/json"},
                            body=json.dumps({"username": user, "password": password}))
    check("una vez bloqueada la IP, ni la password correcta pasa (rate limit corre antes que la DB)",
          status == 429, f"status={status}")


def main():
    print(f"Esperando a que {HOST}:{PORT}/healthz responda...")
    if not wait_for_healthy():
        print("El backend nunca respondió /healthz — abortando.")
        sys.exit(1)

    test_health_and_basic()
    test_404_never_spa_for_api()
    test_static_docs()
    user, password, token = test_auth_flow()
    sub = test_protected_routes(token)
    test_idor_guardrail(token, sub)
    test_oversized_body_413()
    test_smuggling_rejected()
    test_login_rate_limit()  # último a propósito

    total = len(results)
    failed = [r for r in results if not r[1]]
    print(f"\n{total - len(failed)}/{total} tests ok")
    if failed:
        print("\nFallaron:")
        for name, _, detail in failed:
            print(f"  - {name} ({detail})")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
