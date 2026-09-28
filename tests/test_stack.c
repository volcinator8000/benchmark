#include "../src/stack.h"
#include "test_util.h"

int main(void) {
    Stack *s = stack_new();
    CHECK(stack_is_empty(s));
    CHECK(stack_pop(s) == NULL);
    CHECK(stack_peek(s) == NULL);

    int vals[100];
    for (int i = 0; i < 100; i++) { vals[i] = i; stack_push(s, &vals[i]); }
    CHECK(stack_size(s) == 100);
    CHECK(*(int *)stack_peek(s) == 99);
    CHECK(*(int *)stack_at(s, 0) == 99);
    CHECK(*(int *)stack_at(s, 99) == 0);
    CHECK(stack_at(s, 100) == NULL);

    for (int i = 99; i >= 0; i--) CHECK(*(int *)stack_pop(s) == i);
    CHECK(stack_is_empty(s));
    CHECK(stack_pop(s) == NULL);

    stack_push(s, &vals[1]);
    stack_clear(s);
    CHECK(stack_size(s) == 0);
    stack_free(s);

    /* stack_free_all frees owned elements */
    Stack *o = stack_new();
    for (int i = 0; i < 5; i++) stack_push(o, malloc(16));
    stack_free_all(o, free);

    TEST_MAIN_END();
}
