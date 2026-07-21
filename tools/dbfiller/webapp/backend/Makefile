# Configuración de Compilador
CC       := gcc
CFLAGS   := -O3 -Wall -Wextra -D_GNU_SOURCE -static
LDFLAGS  := -static -L/usr/lib -L/lib

# Directorios
BUILD_DIR := build
BIN_NAME  := app

# --- BUSQUEDA RECURSIVA ---
# Todo el codigo fuente vive bajo src/ (Docker/Makefile/README quedan
# afuera a proposito, ver la raiz de este proyecto) -- se busca solo ahi,
# no en todo el directorio del proyecto.
SRCS := $(shell find src -name "*.c" ! -path "$(BUILD_DIR)/*")
# Genera la lista de objetos en la carpeta build preservando estructura
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

# Búsqueda de headers: Incluimos todas las carpetas que contienen archivos .h
# Esto evita tener que usar rutas relativas largas como ../../ en los #include
INCLUDES := $(sort $(dir $(shell find src -name "*.h" ! -path "$(BUILD_DIR)/*")))
INCLUDE_FLAGS := -Isrc $(addprefix -I, $(INCLUDES)) -I/usr/include/postgresql -I/usr/include

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