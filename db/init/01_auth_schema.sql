-- Tabla de usuarios para autenticacion propia (JWT HS256, ver
-- utils/auth/jwt.c y controllers/auth.c). Separada a proposito de
-- db/sakila/ (dataset de ejemplo): esta tabla es parte del boilerplate
-- en si, no del dataset de prueba, y sobrevive si Sakila se reemplaza
-- por el modelo real de un cliente.
CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    username TEXT NOT NULL UNIQUE,
    -- Formato "salt_hex:hash_hex" (ver utils/auth/password.c), nunca la
    -- contrasena en texto plano.
    password_hash TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
