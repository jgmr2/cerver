CC ?= gcc

SRC := src/main.c src/introspect.c src/codegen.c src/repo_patch.c
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
