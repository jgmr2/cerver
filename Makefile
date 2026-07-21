# Configuración de Compilador
# Flags de hardening (ver TODO.md — probadas contra el build real en
# Alpine/musl, no asumidas):
#   -fstack-protector-strong   canarios de pila en funciones con arrays/
#                              structs locales expuestos a direccion tomada
#   -D_FORTIFY_SOURCE=2        chequeo de tamano en funciones de string/mem
#                              de la libc (memcpy, snprintf, etc) cuando el
#                              tamano del buffer es conocido en compilacion;
#                              requiere -O1+ (ya hay -O3)
#   -Wl,-z,relro,-z,now        relocaciones de solo lectura tras el arranque
CC       := gcc
CFLAGS   := -O3 -Wall -Wextra -D_GNU_SOURCE -static -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS  := -static -Wl,-z,relro,-z,now -L/usr/lib -L/lib

# Directorios
BUILD_DIR := build
BIN_NAME  := app

# --- BUSQUEDA RECURSIVA ---
# Encuentra todos los archivos .c en el directorio actual y subdirectorios.
# tools/ y tests/ quedan afuera: tools/dbfiller y tests/unit son binarios
# aparte con su propio main() (tools/dbfiller/Makefile,
# tests/unit/Makefile) - incluirlos aca choca con el main() de este binario.
SRCS := $(shell find . -name "*.c" ! -path "./$(BUILD_DIR)/*" ! -path "./tools/*" ! -path "./tests/*")
# Genera la lista de objetos en la carpeta build preservando estructura
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

# Búsqueda de headers: Incluimos todas las carpetas que contienen archivos .h
# Esto evita tener que usar rutas relativas largas como ../../ en los #include
INCLUDES := $(sort $(dir $(shell find . -name "*.h" ! -path "./$(BUILD_DIR)/*" ! -path "./tools/*" ! -path "./tests/*")))
INCLUDE_FLAGS := -I. $(addprefix -I, $(INCLUDES)) -I/usr/include/postgresql -I/usr/include

# Librerías
PG_LIBS := -Wl,--start-group -lpq -lpgcommon -lpgport -Wl,--end-group
LIBS    := $(PG_LIBS) -luring -lssl -lcrypto -lz -lpthread -ldl -lm

.PHONY: all clean debug

all: $(BIN_NAME)

# Enlace final
$(BIN_NAME): $(OBJS)
	@echo "🔗 Enlazando binario estático: $(BIN_NAME)..."
	$(CC) $(OBJS) -o $(BIN_NAME) $(LDFLAGS) $(LIBS)
	@strip $(BIN_NAME)
	@echo "✅ Build exitoso. Tamaño del binario:"
	@ls -lh $(BIN_NAME)

# Compilación de objetos (.c -> .o)
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "🔨 Compilando: $<"
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -c $< -o $@

clean:
	@echo "🧹 Limpiando..."
	rm -rf $(BUILD_DIR) $(BIN_NAME)

# Tip: Agregué una regla de debug por si necesitas trackear símbolos
debug: CFLAGS := -g -Wall -D_GNU_SOURCE
debug: all