CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -Isrc
SRC     = src/ast.c src/error.c src/lexer.c src/parser.c src/main.c
OBJ     = $(SRC:.c=.o)
BIN     = penguinc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
