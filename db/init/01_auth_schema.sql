-- Tabla de usuarios para autenticacion propia (JWT HS256, ver
-- utils/auth/jwt.c y utils/auth/auth.c). Es parte del boilerplate en
-- si (infraestructura reutilizable), no del esquema de negocio de un
-- proyecto en particular: el resto de las tablas las agrega cada
-- proyecto en sus propios scripts bajo db/init/, y tools/dbfiller las
-- lee de ahi para generar sus endpoints CRUD (ver tools/dbfiller/README.md).
CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    -- Formato "salt_hex:hash_hex" (ver utils/auth/password.c), nunca la
    -- contrasena en texto plano.
    password_hash TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
