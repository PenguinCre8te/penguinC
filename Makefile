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

stdlib: $(STDLIB)/io/io.o $(STDLIB)/threads/threads.o runtime/arc.o

$(STDLIB)/io/io.o: $(STDLIB)/io/io.c runtime/arc.c
	$(CC) -Wall -Wextra -std=c11 -g -Iruntime -c -o $@ $<

$(STDLIB)/threads/threads.o: $(STDLIB)/threads/threads.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $< -lpthread

runtime/arc.o: runtime/arc.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

test: $(BIN) stdlib
	@./tests/run_tests.sh

test-%: $(BIN) stdlib
	@./tests/run_tests.sh $*

clean:
	rm -f $(OBJ) $(BIN) $(STDLIB)/io/*.o $(STDLIB)/threads/*.o runtime/*.o

.PHONY: all clean test stdlib
