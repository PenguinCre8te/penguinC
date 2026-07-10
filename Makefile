CC       = gcc
LLVM_CFLAGS = $(shell llvm-config --cflags 2>/dev/null)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs core native --system-libs 2>/dev/null)
CFLAGS   = -Wall -Wextra -std=c11 -g -Isrc -Isrc/frontend -Isrc/codegen $(LLVM_CFLAGS)

FRONTEND = src/frontend/ast.c src/frontend/lexer.c src/frontend/parser.c \
           src/frontend/typecheck.c src/frontend/printast.c
CODEGEN  = src/codegen/ctx.c src/codegen/arc.c src/codegen/mangle.c \
           src/codegen/type.c src/codegen/import.c src/codegen/expr.c \
           src/codegen/stmt.c src/codegen/decl.c src/codegen/backend.c
ROOT     = src/error.c src/main.c
SRC      = $(FRONTEND) $(CODEGEN) $(ROOT)
OBJ      = $(patsubst src/%.c,src/build/%.o,$(SRC))
BIN      = penguinc
STDLIB   = stdlib

all: $(BIN) stdlib

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LLVM_LDFLAGS)

src/build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

src/build/frontend/%.o: src/frontend/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

src/build/codegen/%.o: src/codegen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

stdlib: $(STDLIB)/console/console.o $(STDLIB)/threads/threads.o $(STDLIB)/mutex/mutex.o $(STDLIB)/files/files.o runtime/arc.o

$(STDLIB)/console/console.o: $(STDLIB)/console/console.c runtime/arc.c
	$(CC) -Wall -Wextra -std=c11 -g -Iruntime -c -o $@ $<

$(STDLIB)/threads/threads.o: $(STDLIB)/threads/threads.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $< -lpthread

$(STDLIB)/mutex/mutex.o: $(STDLIB)/mutex/mutex.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $< -lpthread

$(STDLIB)/files/files.o: $(STDLIB)/files/files.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

runtime/arc.o: runtime/arc.c
	$(CC) -Wall -Wextra -std=c11 -g -c -o $@ $<

test: $(BIN) stdlib
	@./tests/run_tests.sh
	@./tests/run_errors.sh

test-errors: $(BIN) stdlib
	@./tests/run_errors.sh

test-%: $(BIN) stdlib
	@./tests/run_tests.sh $*

clean:
	rm -rf src/build $(BIN) $(STDLIB)/console/*.o $(STDLIB)/threads/*.o $(STDLIB)/mutex/*.o $(STDLIB)/files/*.o runtime/*.o

.PHONY: all clean test test-errors stdlib
