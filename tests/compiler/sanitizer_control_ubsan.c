#include <limits.h>

int main(void) {
    volatile int max_value = INT_MAX;
    volatile int increment = 1;
    volatile int overflow = max_value + increment;
    return overflow == 0;
}
