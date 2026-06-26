CC       = gcc
LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core native --system-libs 2>/dev/null)
CFLAGS   = -Wall -Wextra -std=c11 -g -Isrc $(LLVM_CFLAGS)
SRC      = src/ast.c src/error.c src/lexer.c src/parser.c src/codegen.c src/main.c
OBJ      = $(SRC:.c=.o)
BIN      = penguinc
STDLIB   = stdlib

all: $(BIN) stdlib

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

stdlib: $(STDLIB)/io/io.o $(STDLIB)/mem/mem.o

$(STDLIB)/io/io.o: $(STDLIB)/io/io.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

$(STDLIB)/mem/mem.o: $(STDLIB)/mem/mem.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

test: $(BIN) stdlib
	@./tests/run_tests.sh

test-%: $(BIN) stdlib
	@./tests/run_tests.sh $*

clean:
	rm -f $(OBJ) $(BIN) $(STDLIB)/io/*.o $(STDLIB)/mem/*.o

.PHONY: all clean test stdlib
