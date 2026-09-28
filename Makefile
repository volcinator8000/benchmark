CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
SRC      = src/buf.c src/stack.c src/json.c src/tmpl.c src/store.c src/http.c src/main.c
OBJ      = $(SRC:.c=.o)
PORT    ?= 8080

board: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lm

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: board
	./board $(PORT)

test: src/buf.o src/stack.o src/json.o src/tmpl.o tests/test_tmpl.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_tmpl tests/test_tmpl.c src/buf.o src/stack.o src/json.o src/tmpl.o -lm
	./tests/test_tmpl

clean:
	rm -f board src/*.o tests/test_tmpl

.PHONY: run test clean
