CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
BIN     := bin/rmm-gen

.PHONY: all clean
all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -rf bin
