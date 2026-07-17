DROP TABLE IF EXISTS pedidos;
DROP TABLE IF EXISTS usuarios;

CREATE TABLE usuarios (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    nombre VARCHAR(60) NOT NULL,
    email VARCHAR(100) NOT NULL UNIQUE,
    edad INTEGER,
    activo BOOLEAN NOT NULL,
    rol TEXT NOT NULL CHECK (rol IN ('admin', 'cliente', 'soporte')),
    creado_en DATETIME NOT NULL
);

CREATE TABLE pedidos (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    usuario_id INTEGER NOT NULL REFERENCES usuarios(id),
    total REAL NOT NULL,
    estado TEXT NOT NULL CHECK (estado IN ('pendiente', 'pagado', 'cancelado')),
    fecha DATE NOT NULL
);

INSERT INTO usuarios (nombre, email, edad, activo, rol, creado_en)
VALUES ('Semilla Inicial', 'semilla@example.com', 30, 1, 'admin', '2024-01-01 00:00:00');
