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

stdlib: $(STDLIB)/io.o $(STDLIB)/io_aliases.o $(STDLIB)/mem.o $(STDLIB)/mem_aliases.o

$(STDLIB)/%.o: $(STDLIB)/%.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

$(STDLIB)/%.o: $(STDLIB)/%.s
	$(CC) -c $< -o $@

test: $(BIN) stdlib
	@./tests/run_tests.sh

test-%: $(BIN) stdlib
	@./tests/run_tests.sh $*

clean:
	rm -f $(OBJ) $(BIN) $(STDLIB)/*.o

.PHONY: all clean test stdlib
