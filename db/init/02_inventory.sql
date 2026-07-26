-- Script de creación de base de datos optimizado para PostgreSQL
-- Incluye correcciones de sintaxis, tipos de datos enriquecidos, restricciones de clave foránea y sugerencias planteadas.

BEGIN;

-- -----------------------------------------------------
-- 1. EXTENSIONES Y FUNCIONES DE UTILIDAD
-- -----------------------------------------------------
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Función para actualizar campos updatedAt automáticamente
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

-- -----------------------------------------------------
-- 2. ENUMS (ESTADOS Y TIPOS)
-- -----------------------------------------------------
CREATE TYPE user_status_enum AS ENUM ('active', 'inactive', 'suspended', 'pending');
CREATE TYPE item_status_enum AS ENUM ('available', 'in_use', 'maintenance', 'retired', 'damaged');
CREATE TYPE report_status_enum AS ENUM ('open', 'in_progress', 'resolved', 'closed', 'cancelled');
CREATE TYPE report_priority_enum AS ENUM ('low', 'medium', 'high', 'critical');
CREATE TYPE request_status_enum AS ENUM ('pending', 'approved', 'rejected', 'in_fulfillment', 'completed', 'cancelled');
CREATE TYPE notification_type_enum AS ENUM ('info', 'warning', 'alert', 'system');

-- -----------------------------------------------------
-- 3. TABLAS INDEPENDIENTES / DE REFERENCIA
-- -----------------------------------------------------

-- Roles
CREATE TABLE role (
    id SERIAL PRIMARY KEY,
    name VARCHAR(50) UNIQUE NOT NULL,
    description TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Permisos
CREATE TABLE permission (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL,
    details TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Tabla pivote: Permisos por Rol (nombre corregido: fk_permission)
CREATE TABLE role_permission (
    id SERIAL PRIMARY KEY,
    fk_role INT NOT NULL REFERENCES role(id) ON DELETE CASCADE,
    fk_permission INT NOT NULL REFERENCES permission(id) ON DELETE CASCADE,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT uq_role_permission UNIQUE (fk_role, fk_permission)
);

-- Departamentos / Áreas (reemplaza texto libre de career/area)
CREATE TABLE department (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL,
    description TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Categorías de Ítems / Activos
CREATE TABLE item_category (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL,
    description TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Ubicaciones / Espacios
CREATE TABLE place (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    description TEXT,
    class_type VARCHAR(50),
    ip_range VARCHAR(50),
    location TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- -----------------------------------------------------
-- 4. USUARIOS
-- -----------------------------------------------------
CREATE TABLE "user" (
    id SERIAL PRIMARY KEY,
    first_name VARCHAR(50) NOT NULL,
    last_name VARCHAR(50) NOT NULL,
    status user_status_enum NOT NULL DEFAULT 'active',
    auth_code VARCHAR(100) UNIQUE,
    email VARCHAR(150) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    fk_department INT REFERENCES department(id) ON DELETE SET NULL,
    fk_role INT NOT NULL REFERENCES role(id) ON DELETE RESTRICT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- -----------------------------------------------------
-- 5. ÍTEMS / ACTIVOS
-- -----------------------------------------------------
CREATE TABLE item (
    id SERIAL PRIMARY KEY,
    name VARCHAR(150) NOT NULL,
    description TEXT,
    cost NUMERIC(12, 2) DEFAULT 0.00,
    manufacturer VARCHAR(100),
    code_bar VARCHAR(100) UNIQUE,
    status item_status_enum NOT NULL DEFAULT 'available',
    fk_category INT REFERENCES item_category(id) ON DELETE SET NULL,
    fk_user_responsible INT REFERENCES "user"(id) ON DELETE SET NULL,
    fk_place INT REFERENCES place(id) ON DELETE SET NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- -----------------------------------------------------
-- 6. REPORTES Y SOLICITUDES
-- -----------------------------------------------------

-- Reportes de incidencias
CREATE TABLE report (
    id SERIAL PRIMARY KEY,
    name VARCHAR(150) NOT NULL,
    description TEXT NOT NULL,
    status report_status_enum NOT NULL DEFAULT 'open',
    priority report_priority_enum NOT NULL DEFAULT 'medium',
    due_date TIMESTAMP WITH TIME ZONE,
    fk_user INT NOT NULL REFERENCES "user"(id) ON DELETE RESTRICT,
    fk_place INT REFERENCES place(id) ON DELETE SET NULL,
    fk_item INT REFERENCES item(id) ON DELETE SET NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Comentarios / Seguimiento de Reportes (Sugerencia añadida)
CREATE TABLE report_comment (
    id SERIAL PRIMARY KEY,
    fk_report INT NOT NULL REFERENCES report(id) ON DELETE CASCADE,
    fk_user INT NOT NULL REFERENCES "user"(id) ON DELETE RESTRICT,
    comment TEXT NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Solicitudes (Requests)
CREATE TABLE request (
    id SERIAL PRIMARY KEY,
    name VARCHAR(150) NOT NULL,
    description TEXT NOT NULL,
    status request_status_enum NOT NULL DEFAULT 'pending',
    fk_item INT REFERENCES item(id) ON DELETE SET NULL,
    fk_user_requester INT NOT NULL REFERENCES "user"(id) ON DELETE RESTRICT,
    fk_user_receiver INT REFERENCES "user"(id) ON DELETE SET NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- -----------------------------------------------------
-- 7. HISTORIAL Y NOTIFICACIONES
-- -----------------------------------------------------

-- Historial / Auditoría
CREATE TABLE history (
    id SERIAL PRIMARY KEY,
    action_type VARCHAR(50) NOT NULL,
    description TEXT,
    fk_user INT REFERENCES "user"(id) ON DELETE SET NULL,
    fk_item INT REFERENCES item(id) ON DELETE SET NULL,
    fk_place INT REFERENCES place(id) ON DELETE SET NULL,
    fk_report INT REFERENCES report(id) ON DELETE SET NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Notificaciones (Aclaradas llaves foráneas: trigger_user y target_user)
CREATE TABLE notification (
    id SERIAL PRIMARY KEY,
    description TEXT NOT NULL,
    type notification_type_enum NOT NULL DEFAULT 'info',
    is_read BOOLEAN NOT NULL DEFAULT FALSE,
    fk_trigger_user INT REFERENCES "user"(id) ON DELETE SET NULL, -- Usuario que generó la acción
    fk_target_user INT NOT NULL REFERENCES "user"(id) ON DELETE CASCADE, -- Usuario receptor de la notificación
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- -----------------------------------------------------
-- 8. TRIGGERS DE ACTUALIZACIÓN DE TIMESTAMP
-- -----------------------------------------------------
CREATE TRIGGER trg_role_updated_at BEFORE UPDATE ON role FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
CREATE TRIGGER trg_place_updated_at BEFORE UPDATE ON place FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
CREATE TRIGGER trg_user_updated_at BEFORE UPDATE ON "user" FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
CREATE TRIGGER trg_item_updated_at BEFORE UPDATE ON item FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
CREATE TRIGGER trg_report_updated_at BEFORE UPDATE ON report FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();
CREATE TRIGGER trg_request_updated_at BEFORE UPDATE ON request FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

-- -----------------------------------------------------
-- 9. ÍNDICES DE RENDIMIENTO
-- -----------------------------------------------------
CREATE INDEX idx_user_email ON "user"(email);
CREATE INDEX idx_user_role ON "user"(fk_role);
CREATE INDEX idx_item_code_bar ON item(code_bar);
CREATE INDEX idx_item_status ON item(status);
CREATE INDEX idx_item_place ON item(fk_place);
CREATE INDEX idx_report_status ON report(status);
CREATE INDEX idx_report_user ON report(fk_user);
CREATE INDEX idx_request_status ON request(status);
CREATE INDEX idx_history_user ON history(fk_user);
CREATE INDEX idx_notification_target ON notification(fk_target_user, is_read);

COMMIT;
