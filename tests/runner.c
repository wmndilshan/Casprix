#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
    #define COMPILER_BIN ".\\build\\casprix.exe"
    #define PATH_SEP "\\"
#else
    #define COMPILER_BIN "./build/casprix"
    #define PATH_SEP "/"
#endif

#define TEST_DIR "tests/compiler"

int main() {
    DIR *dir;
    struct dirent *ent;
    int passed = 0;
    int failed = 0;
    int total = 0;
    clock_t start = clock();

    const char* compiler_bin = getenv("CASPRIX_EXE");
    if (!compiler_bin) {
        compiler_bin = COMPILER_BIN;
    }

#ifdef _WIN32
    const char* null_dev = "nul";
#else
    const char* null_dev = "/dev/null";
#endif

    printf("========================================\n");
    printf("Casprix Compiler Test Suite (C Runner)\n");
    printf("========================================\n\n");

    if ((dir = opendir(TEST_DIR)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strstr(ent->d_name, ".cpx") != NULL) {
                total++;
                char cmd[512];
                char path[512];
                snprintf(path, sizeof(path), "%s%s%s", TEST_DIR, PATH_SEP, ent->d_name);
                snprintf(cmd, sizeof(cmd), "%s --check-only %s > %s 2>&1", compiler_bin, path, null_dev);

                printf("[%2d] Testing %-40s... ", total, ent->d_name);
                fflush(stdout);

                int res = system(cmd);
                if (res == 0) {
                    printf("PASSED\n");
                    passed++;
                } else {
                    printf("FAILED (code %d)\n", res);
                    failed++;
                }
            }
        }
        closedir(dir);
    } else {
        perror("Could not open test directory");
        return 1;
    }

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    printf("\n----------------------------------------\n");
    printf("Results:\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    printf("  Time:   %.2f seconds\n", time_spent);
    printf("----------------------------------------\n");

    return (failed > 0) ? 1 : 0;
}
