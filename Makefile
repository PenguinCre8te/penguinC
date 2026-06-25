CC       = gcc
LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core native --system-libs 2>/dev/null)
CFLAGS   = -Wall -Wextra -std=c11 -g -Isrc $(LLVM_CFLAGS)
SRC      = src/ast.c src/error.c src/lexer.c src/parser.c src/codegen.c src/main.c
OBJ      = $(SRC:.c=.o)
BIN      = penguinc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
