/* stack.h - a small generic LIFO stack of void* pointers.
 *
 * Used by the template compiler (block nesting: #each / #if / else)
 * and by the template renderer (scope/context stack).
 */
#ifndef STACK_H
#define STACK_H

#include <stddef.h>

typedef struct {
    void  **items;
    size_t  len;
    size_t  cap;
} Stack;

void   stack_init(Stack *s);
void   stack_free(Stack *s);            /* frees the array, not the items */
int    stack_push(Stack *s, void *item); /* 0 on success, -1 on OOM */
void  *stack_pop(Stack *s);             /* NULL when empty */
void  *stack_peek(const Stack *s);      /* NULL when empty */
void  *stack_at(const Stack *s, size_t depth_from_top); /* 0 == top */
size_t stack_len(const Stack *s);
int    stack_empty(const Stack *s);

#endif
