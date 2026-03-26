#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

// Memory allocation macros
#define ALLOCATE(type, count) \
    (type*)malloc(sizeof(type) * (count))

#define GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
    (type*)realloc(pointer, sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, oldCount) \
    free(pointer)

// Utility macros
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif // COMMON_H