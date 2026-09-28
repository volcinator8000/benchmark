CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
LDFLAGS ?=
LDLIBS   = -lm

SRC_LIB  = src/util.c src/stack.c src/json.c src/template.c src/http.c src/store.c
SRC_MAIN = src/main.c
BIN      = widgets
TEST_CFLAGS = $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
TESTS    = tests/test_stack tests/test_json tests/test_template

.PHONY: all run test clean

all: $(BIN)

$(BIN): $(SRC_LIB) $(SRC_MAIN) src/*.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_LIB) $(SRC_MAIN) $(LDLIBS)

run: $(BIN)
	./$(BIN)

# Server binary with AddressSanitizer/UBSan for end-to-end checks.
$(BIN)-asan: $(SRC_LIB) $(SRC_MAIN) src/*.h
	$(CC) $(TEST_CFLAGS) $(LDFLAGS) -o $@ $(SRC_LIB) $(SRC_MAIN) $(LDLIBS)

tests/test_stack: tests/test_stack.c src/stack.c src/util.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_stack.c src/stack.c src/util.c $(LDLIBS)

tests/test_json: tests/test_json.c src/json.c src/util.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_json.c src/json.c src/util.c $(LDLIBS)

tests/test_template: tests/test_template.c src/template.c src/json.c src/stack.c src/util.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_template.c src/template.c src/json.c src/stack.c src/util.c $(LDLIBS)

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(BIN) $(BIN)-asan $(TESTS)
