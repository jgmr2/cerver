CC ?= gcc

# Compartido por el CLI y la GUI: toda la logica real (introspeccion,
# codegen, patch de routes/registry, orquestacion) vive aca, sin nada
# atado a stdio ni a argv - ver src/generate.h.
CORE_SRC := src/introspect.c src/codegen.c src/repo_patch.c src/generate.c src/scaffold.c

SRC := src/main.c $(CORE_SRC)
CFLAGS := -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I/usr/include/postgresql -I/usr/include
LDFLAGS := -lpq

GUI_SRC := src/gui/gui_main.c $(CORE_SRC)

BUILD_DIR := build
OUT := $(BUILD_DIR)/dbfiller
GUI_OUT := $(BUILD_DIR)/dbfiller-gui

.PHONY: all gui clean

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)
	@echo "Binario: $(OUT)"

# Requiere libgtk-4-dev (ver "Interfaz grafica" en README.md). El CLI
# (target 'all', arriba) no depende de GTK en absoluto.
gui: $(GUI_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -I/usr/include/postgresql -I/usr/include $$(pkg-config --cflags gtk4) $(GUI_SRC) -o $(GUI_OUT) $$(pkg-config --libs gtk4) -lpq
	@echo "Binario: $(GUI_OUT)"

clean:
	rm -rf $(BUILD_DIR)
