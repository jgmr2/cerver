# Configuración de Compilador
CC       := gcc
# Agregamos -static aquí también para asegurar consistencia
CFLAGS   := -O3 -Wall -Wextra -D_GNU_SOURCE -static
LDFLAGS  := -static -L/usr/lib -L/lib

# Directorios
SRC_DIR   := .
BUILD_DIR := build
BIN_NAME  := app

# Búsqueda de headers
# Incluimos el directorio raíz y las carpetas de postgres
INCLUDE_FLAGS := -I. -I/usr/include/postgresql -I/usr/include

# --- CORRECCIÓN LIBRERÍAS (ORDEN CRÍTICO) ---
# Al ser estático, el orden importa: quien usa una función va a la izquierda de quien la provee.
# 1. libpq necesita a ssl y crypto.
# 2. libuv necesita lpthread y dl.
PG_LIBS := -Wl,--start-group -lpq -lpgcommon -lpgport -Wl,--end-group
LIBS := $(PG_LIBS) -luv -lssl -lcrypto -lz -lpthread -ldl -lm

# Archivos de origen (Tu estructura específica)
SRCS := main.c config/db.c
# Esto convierte main.c en build/main.o y config/db.c en build/config/db.o
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BIN_NAME)

# Enlace final
$(BIN_NAME): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "🔗 Enlazando binario estático final..."
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)
	@echo "--------------------------------------------"
	@echo "Build exitoso: $(BIN_NAME) (C Estático)"
	@strip $(BIN_NAME) 
	@ls -lh $(BIN_NAME)

# Compilación de objetos
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_NAME)