#include "stack.h"
#include "util.h"

#include <stdlib.h>

struct Stack {
    void **items;
    size_t len;
    size_t cap;
};

Stack *stack_new(void) {
    Stack *s = xcalloc(1, sizeof *s);
    s->cap = 8;
    s->items = xmalloc(s->cap * sizeof *s->items);
    return s;
}

void stack_free(Stack *s) {
    if (!s) return;
    free(s->items);
    free(s);
}

void stack_free_all(Stack *s, void (*dtor)(void *)) {
    if (!s) return;
    if (dtor)
        for (size_t i = 0; i < s->len; i++) dtor(s->items[i]);
    stack_free(s);
}

void stack_push(Stack *s, void *item) {
    if (s->len == s->cap) {
        s->cap *= 2;
        s->items = xrealloc(s->items, s->cap * sizeof *s->items);
    }
    s->items[s->len++] = item;
}

void *stack_pop(Stack *s) {
    if (s->len == 0) return NULL;
    return s->items[--s->len];
}

void *stack_peek(const Stack *s) {
    return s->len ? s->items[s->len - 1] : NULL;
}

void *stack_at(const Stack *s, size_t depth) {
    if (depth >= s->len) return NULL;
    return s->items[s->len - 1 - depth];
}

size_t stack_size(const Stack *s) {
    return s->len;
}

int stack_is_empty(const Stack *s) {
    return s->len == 0;
}

void stack_clear(Stack *s) {
    s->len = 0;
}
