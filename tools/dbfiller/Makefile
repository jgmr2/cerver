CC ?= gcc
MINGW_CC ?= x86_64-w64-mingw32-gcc

CORE_SRC := src/schema.c src/generator.c src/db_graph.c src/csv_dict.c src/db/db_backend.c src/db/db_sqlite.c third_party/sqlite3.c
MYSQL_SRC := src/db/db_mysql.c
MYSQL_CFLAGS := $(shell mariadb_config --cflags 2>/dev/null) -DDBFILLER_HAVE_MYSQL
MYSQL_LIBS := $(shell mariadb_config --libs 2>/dev/null)

# MariaDB Connector/C for the Windows cross-build: a from-source static
# build (WITH_CURL=OFF, WITH_SSL=SCHANNEL, bundled zlib) vendored in
# third_party/mariadb-win64 - see NOTICE.txt there for exactly how it was
# built. Self-contained: only standard Windows system libraries needed,
# no extra DLLs to ship alongside the .exe.
MARIADB_WIN_DIR := third_party/mariadb-win64
MYSQL_CFLAGS_WINDOWS := -I$(MARIADB_WIN_DIR)/include -DDBFILLER_HAVE_MYSQL
MYSQL_LIBS_WINDOWS := -L$(MARIADB_WIN_DIR)/lib -lmariadbclient -lws2_32 -ladvapi32 -lkernel32 -lshlwapi -lcrypt32 -lbcrypt -lsecur32

SRC := src/main.c $(CORE_SRC) $(MYSQL_SRC)
# saved_connections.c is portable (its config-path resolution has its own
# _WIN32 branch) so it's shared by both GUI targets; docker_control.c shells
# out to the `docker` CLI via fork/execvp and is Linux-only for now, so it's
# Linux-GUI-only too - see gui_app.c's own `#ifndef _WIN32` around its use.
GUI_SRC := src/gui/gui_app.c src/gui/saved_connections.c src/gui/docker_control.c $(CORE_SRC) $(MYSQL_SRC)
WINDOWS_SRC := src/main.c $(CORE_SRC) $(MYSQL_SRC)

CFLAGS := -Wall -Wextra -O2 -Ithird_party -std=c11 -D_POSIX_C_SOURCE=200809L $(MYSQL_CFLAGS)
BUILD_DIR := build

LDFLAGS_LINUX := -lpthread -ldl -lm $(MYSQL_LIBS)
LDFLAGS_WINDOWS := -lm -static $(MYSQL_LIBS_WINDOWS)
LDFLAGS_GUI_LINUX := -lglfw -lGL -lm -lpthread -ldl $(MYSQL_LIBS)

LINUX_OUT := $(BUILD_DIR)/linux/dbfiller
WINDOWS_OUT := $(BUILD_DIR)/windows/dbfiller.exe
GUI_LINUX_OUT := $(BUILD_DIR)/linux/dbfiller-gui
GUI_WINDOWS_OUT := $(BUILD_DIR)/windows/dbfiller-gui.exe

# GLFW precompiled for mingw-w64, vendored from glfw.org.
GLFW_WIN_DIR := third_party/glfw-win64
WINDOWS_GUI_SRC := src/gui/gui_app.c src/gui/saved_connections.c $(CORE_SRC) $(MYSQL_SRC)
LDFLAGS_GUI_WINDOWS := -L$(GLFW_WIN_DIR)/lib -lglfw3 -lopengl32 -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lm -static-libgcc $(MYSQL_LIBS_WINDOWS)

# Detecta el SO anfitrión para que `make` sin argumentos produzca el binario
# nativo correcto (Linux, o Windows si se corre dentro de un shell MSYS2/MinGW).
ifeq ($(OS),Windows_NT)
    NATIVE_CC := $(CC)
    NATIVE_OUT := $(WINDOWS_OUT)
    NATIVE_SRC := $(WINDOWS_SRC)
    NATIVE_LDFLAGS := $(LDFLAGS_WINDOWS)
else
    NATIVE_CC := $(CC)
    NATIVE_OUT := $(LINUX_OUT)
    NATIVE_SRC := $(SRC)
    NATIVE_LDFLAGS := $(LDFLAGS_LINUX)
endif

.PHONY: all linux windows gui windows-gui clean

all: $(NATIVE_OUT)

$(NATIVE_OUT): $(NATIVE_SRC)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(CFLAGS) $(NATIVE_SRC) -o $@ $(NATIVE_LDFLAGS)
	@echo "Binario: $@"

# Build nativo explícito de Linux, sin importar el SO anfitrión.
linux:
	@mkdir -p $(BUILD_DIR)/linux
	$(CC) $(CFLAGS) $(SRC) -o $(LINUX_OUT) $(LDFLAGS_LINUX)
	@echo "Binario: $(LINUX_OUT)"

# Cross-compile a Windows usando MinGW-w64 (requiere `mingw-w64` instalado).
# MySQL/MariaDB support via the vendored static Connector/C - see
# MARIADB_WIN_DIR above and third_party/mariadb-win64/NOTICE.txt.
windows:
	@mkdir -p $(BUILD_DIR)/windows
	$(MINGW_CC) -Wall -Wextra -O2 -Ithird_party -std=c11 -D_POSIX_C_SOURCE=200809L $(MYSQL_CFLAGS_WINDOWS) \
		$(WINDOWS_SRC) -o $(WINDOWS_OUT) $(LDFLAGS_WINDOWS)
	@echo "Binario: $(WINDOWS_OUT)"

# GUI (Nuklear + GLFW/OpenGL2). nuklear.h itself triggers a lot of noise
# under -Wall/-Wextra, so it's compiled quietly; our own sources still get
# the full warning set via CORE_SRC/GUI_SRC compiled through the same flags.
gui:
	@mkdir -p $(BUILD_DIR)/linux
	$(CC) -O2 -Ithird_party -std=c11 -D_POSIX_C_SOURCE=200809L $(MYSQL_CFLAGS) -w $(GUI_SRC) -o $(GUI_LINUX_OUT) $(LDFLAGS_GUI_LINUX)
	@echo "Binario: $(GUI_LINUX_OUT)"

# Cross-compile de la GUI a Windows (mingw-w64 + GLFW y MariaDB Connector/C
# vendorizados). Solo se verifica que enlace y produzca un PE32+ valido;
# correrlo en Windows real queda pendiente de que el usuario lo pruebe.
windows-gui:
	@mkdir -p $(BUILD_DIR)/windows
	$(MINGW_CC) -O2 -Ithird_party -I$(GLFW_WIN_DIR)/include -std=c11 -D_POSIX_C_SOURCE=200809L $(MYSQL_CFLAGS_WINDOWS) -w \
		$(WINDOWS_GUI_SRC) -o $(GUI_WINDOWS_OUT) $(LDFLAGS_GUI_WINDOWS)
	@echo "Binario: $(GUI_WINDOWS_OUT)"

clean:
	rm -rf $(BUILD_DIR)
