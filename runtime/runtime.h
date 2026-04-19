#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <pthread.h>
    #include <fcntl.h>
#endif

// Common runtime utilities
void* nuwan_malloc(size_t size);
void nuwan_free(void* ptr);
char* nuwan_strdup(const char* str);

void nuwan_print_int(int64_t val);
void nuwan_print_float(double val);
void nuwan_print_bool(bool val);
void nuwan_print_str(const char* val);

#endif // RUNTIME_H

