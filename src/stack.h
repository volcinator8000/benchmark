#ifndef STACK_H
#define STACK_H

#include <stddef.h>

/*
 * Generic LIFO stack of pointers. The stack never owns its elements;
 * callers free them. Used by the template engine for its block and
 * context stacks and by the store for per-widget undo history.
 */
typedef struct Stack Stack;

Stack *stack_new(void);
void stack_free(Stack *s);
/* Frees every element with `dtor` (if non-NULL) and then the stack. */
void stack_free_all(Stack *s, void (*dtor)(void *));

void stack_push(Stack *s, void *item);
/* Returns NULL when empty. */
void *stack_pop(Stack *s);
void *stack_peek(const Stack *s);
/* Element `depth` below the top; depth 0 is the top. NULL if out of range. */
void *stack_at(const Stack *s, size_t depth);
size_t stack_size(const Stack *s);
int stack_is_empty(const Stack *s);
void stack_clear(Stack *s);

#endif
