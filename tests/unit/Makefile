# tests/unit/Makefile - compila y corre los tests unitarios de las
# funciones puras (ver test_pure_functions.c).
#
# Mismos includes/libs que el Makefile principal: router.h arrastra
# liburing.h y openssl aunque este test no llame a nada de auth/red -
# son headers, no eligen que se linkee, asi que hace falta lo mismo que
# el binario real para compilar limpio.
ROOT := ../..
CC := gcc
CFLAGS := -Wall -Wextra -D_GNU_SOURCE \
  -I$(ROOT) -I$(ROOT)/config -I$(ROOT)/controllers -I$(ROOT)/core \
  -I$(ROOT)/models -I$(ROOT)/routes -I$(ROOT)/utils -I$(ROOT)/utils/auth \
  -I$(ROOT)/utils/http -I$(ROOT)/utils/net \
  -I/usr/include/postgresql -I/usr/include
LIBS := -luring -lssl -lcrypto -lpthread -ldl -lm

.PHONY: test clean

test: test_pure_functions
	./test_pure_functions

test_pure_functions: test_pure_functions.c
	$(CC) $(CFLAGS) test_pure_functions.c -o test_pure_functions $(LIBS)

clean:
	rm -f test_pure_functions
