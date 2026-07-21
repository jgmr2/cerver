CC ?= gcc

# Compartido por el CLI y por el backend de tools/dbfiller/webapp/ (via
# symlinks relativos en webapp/backend/dbfiller_core/, ver ese
# directorio): toda la logica real (introspeccion, codegen, patch de
# routes/registry, orquestacion) vive aca, sin nada atado a stdio ni a
# argv - ver src/generate.h. La GUI GTK4 que usaba esto mismo se
# elimino (ver tools/dbfiller/webapp/, reemplazo web).
CORE_SRC := src/introspect.c src/codegen.c src/repo_patch.c src/generate.c src/scaffold.c src/seed.c

SRC := src/main.c $(CORE_SRC)
CFLAGS := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I/usr/include/postgresql -I/usr/include
LDFLAGS := -lpq

BUILD_DIR := build
OUT := $(BUILD_DIR)/dbfiller

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)
	@echo "Binario: $(OUT)"

clean:
	rm -rf $(BUILD_DIR)
