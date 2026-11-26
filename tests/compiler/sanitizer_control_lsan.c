#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void intentionally_leak(void) {
    void* memory = malloc(64);
    if (memory) memset(memory, 0xA5, 64);
}

int main(void) {
    intentionally_leak();
    return 0;
}
