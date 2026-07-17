# dbfiller — Generador Inteligente de Inserciones para Bases de Datos

Programa que se conecta a una base de datos, detecta automáticamente la
estructura de sus tablas (columnas, tipos de dato, llaves primarias y
foráneas, restricciones NOT NULL/ENUM/UNIQUE, etc.) y genera e inserta datos
de prueba coherentes con esa estructura, sin que tengas que escribirlos a
mano.

Incluye dos formas de usarlo:

- **Aplicación de escritorio (GUI)** — ventana con botones, pensada para
  cualquier usuario, sin necesidad de usar la terminal.
- **Línea de comandos (CLI)** — pensada para scripts o uso avanzado.

Motores de base de datos soportados: **SQLite** y **MySQL / MariaDB**.

## Descarga rápida (recomendado para la mayoría de usuarios)

Si solo quieres usar el programa, **no hace falta compilar nada**: descarga
el binario ya compilado para tu sistema operativo desde la carpeta
[`build/`](build/) de este repositorio:

| Sistema | Archivo |
|---|---|
| Windows | `build/windows/dbfiller-gui.exe` (con ventana) o `build/windows/dbfiller.exe` (terminal) |
| Linux | `build/linux/dbfiller-gui` (con ventana) o `build/linux/dbfiller` (terminal) |

En Windows basta con hacer doble clic sobre el `.exe`. En Linux, dale
permiso de ejecución y ábrelo:

```bash
chmod +x dbfiller-gui
./dbfiller-gui
```

## Cómo usar la aplicación de escritorio

1. Abre `dbfiller-gui`.
2. Elige el **Motor** de base de datos: `SQLite` o `MySQL/MariaDB`.
   - Para SQLite: escribe la ruta a tu archivo `.db`/`.sqlite` o usa
     "Examinar..." para buscarlo en tus carpetas.
   - Para MySQL/MariaDB: llena Host, Puerto, Usuario y Contraseña del
     servidor al que te quieres conectar.
3. Presiona **Conectar**. La aplicación leerá automáticamente las tablas
   existentes y las mostrará en la barra lateral.
4. Selecciona una tabla para ver su **Estructura** (columnas y tipos) o sus
   **Registros** actuales.
5. Escribe cuántas filas quieres generar y presiona **Generar** para
   llenar esa tabla, o usa **Llenar toda la base de datos** para generar
   datos en todas las tablas respetando el orden correcto según sus
   relaciones (llaves foráneas).
6. También hay una pestaña **SQL** para ejecutar consultas propias, y un
   botón **Vaciar base de datos** (con confirmación) para borrar todos los
   registros si quieres empezar de cero.

## Cómo usar la línea de comandos

```bash
# Contra un archivo SQLite
dbfiller --sqlite ruta/a/mi_base.db --count 100

# Contra una sola tabla de esa base
dbfiller --sqlite ruta/a/mi_base.db --table clientes --count 50

# Contra un servidor MySQL/MariaDB
dbfiller --mysql --host 127.0.0.1 --port 3306 \
         --user root --password secreta --database tienda --count 200
```

- Si no se indica `--table`, se generan datos para **todas** las tablas de
  la base, en el orden correcto según sus relaciones.
- `--count` indica cuántos registros generar por tabla.
- `dbfiller --help` muestra esta misma ayuda en pantalla.

## Compilar desde el código fuente

Solo necesario si quieres modificar el programa o no existe un binario
para tu sistema.

Requisitos (Linux):
- `gcc`
- Encabezados de desarrollo de MariaDB/MySQL (paquete `libmariadb-dev` o
  equivalente) si quieres soporte para MySQL — es opcional, el programa
  compila igual sin ellos y solo quedará disponible SQLite.
- Para la versión con ventana (GUI): `libglfw3-dev` y una librería de
  OpenGL (`libgl1-mesa-dev` o equivalente).

Comandos, desde la raíz del proyecto:

```bash
make            # compila la version de terminal para tu sistema actual
make gui        # compila la version con ventana (Linux)
make windows    # cross-compila la version de terminal para Windows (requiere mingw-w64)
make windows-gui  # cross-compila la version con ventana para Windows
make clean      # borra todo lo compilado (carpeta build/)
```

Los binarios resultantes quedan en `build/linux/` o `build/windows/`.

## Datos de ejemplo

En la carpeta [`data/`](data/) hay archivos CSV de ejemplo
(clientes, organizaciones, personas, productos) usados como fuente de
datos realistas para columnas de texto (nombres, correos, empresas, etc.).

## Licencia

Este proyecto se distribuye bajo la licencia GPLv3. Ver el archivo
[LICENSE](LICENSE) para el texto completo.
