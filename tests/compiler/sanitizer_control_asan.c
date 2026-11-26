#include <stdlib.h>

int main(void) {
    volatile int* value = (int*)malloc(sizeof(int));
    if (!value) return 2;
    *value = 7;
    free((void*)value);
    return *value;
}
