#include "stack.h"

#include <stdlib.h>

void stack_init(Stack *s) {
    s->items = NULL;
    s->len = 0;
    s->cap = 0;
}

void stack_free(Stack *s) {
    free(s->items);
    stack_init(s);
}

int stack_push(Stack *s, void *item) {
    if (s->len == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 8;
        void **n = realloc(s->items, ncap * sizeof *n);
        if (!n) return -1;
        s->items = n;
        s->cap = ncap;
    }
    s->items[s->len++] = item;
    return 0;
}

void *stack_pop(Stack *s) {
    if (s->len == 0) return NULL;
    return s->items[--s->len];
}

void *stack_peek(const Stack *s) {
    return s->len ? s->items[s->len - 1] : NULL;
}

void *stack_at(const Stack *s, size_t depth_from_top) {
    if (depth_from_top >= s->len) return NULL;
    return s->items[s->len - 1 - depth_from_top];
}

size_t stack_len(const Stack *s) { return s->len; }

int stack_empty(const Stack *s) { return s->len == 0; }
