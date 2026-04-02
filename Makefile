# Configuración de Compilador
CC       := gcc
# Mantenemos -static para un binario portable y -D_GNU_SOURCE para io_uring
CFLAGS   := -O3 -Wall -Wextra -D_GNU_SOURCE -static
LDFLAGS  := -static -L/usr/lib -L/lib

# Directorios
SRC_DIR   := .
BUILD_DIR := build
BIN_NAME  := app

# Búsqueda de headers
INCLUDE_FLAGS := -I. -I/usr/include/postgresql -I/usr/include

# --- AJUSTE DE LIBRERÍAS (LIBUV -> LIBURING) ---
# 1. Agrupamos las de Postgres para resolver dependencias circulares entre ellas.
# 2. Reemplazamos -luv por -luring.
# 3. Mantenemos ssl, crypto, z, pthread y dl al final (son dependencias base).
PG_LIBS := -Wl,--start-group -lpq -lpgcommon -lpgport -Wl,--end-group
LIBS    := $(PG_LIBS) -luring -lssl -lcrypto -lz -lpthread -ldl -lm

# Archivos de origen
SRCS := main.c config/db.c
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(BIN_NAME)

# Enlace final
$(BIN_NAME): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "🔗 Enlazando binario estático final con io_uring..."
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)
	@echo "--------------------------------------------"
	@echo "Build exitoso: $(BIN_NAME) (C + io_uring + Estático)"
	@strip $(BIN_NAME) 
	@ls -lh $(BIN_NAME)

# Compilación de objetos
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_NAME)