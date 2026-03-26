// Casprix Standard Library Header
// This file defines the standard library interface for C runtime

#ifndef NUWAN_STDLIB_H
#define NUWAN_STDLIB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// Runtime library functions that will be linked with assembly code
#ifdef __cplusplus
extern "C" {
#endif

// IO functions
int nuwan_printf(const char* format, ...);
int nuwan_scanf(const char* format, ...);

// File operations
void* nuwan_fopen(const char* filename, const char* mode);
int nuwan_fclose(void* file);
int nuwan_fread(void* buffer, size_t size, size_t count, void* file);
int nuwan_fwrite(const void* buffer, size_t size, size_t count, void* file);

// Casprix file API
int nuwan_file_open_read(const char* filename);
int nuwan_file_open_write(const char* filename);
int nuwan_file_close(int handle);
char* nuwan_file_read_line(int handle);
int nuwan_file_write(int handle, const char* content);
bool nuwan_file_exists(const char* filename);
long nuwan_file_size(const char* filename);
int nuwan_file_delete(const char* filename);
int nuwan_file_copy(const char* source, const char* destination);

// String operations
char* nuwan_strconcat(const char* s1, const char* s2);
int nuwan_strlen(const char* str);

// Math operations
long long nuwan_power(long long base, long long exp);
long long nuwan_sqrt(long long n);
long long nuwan_gcd(long long a, long long b);
long long nuwan_lcm(long long a, long long b);

// Thread operations (platform-specific)
#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE nuwan_thread_t;
#else
    #include <pthread.h>
    #include <unistd.h>
    typedef pthread_t nuwan_thread_t;
#endif

nuwan_thread_t nuwan_thread_create(void* (*func)(void*), void* arg);
int nuwan_thread_join(nuwan_thread_t thread);
void nuwan_thread_sleep(int milliseconds);
int nuwan_thread_get_id(void);

// Mutex operations
int nuwan_mutex_create(void);
int nuwan_mutex_lock(int mutex_handle);
int nuwan_mutex_unlock(int mutex_handle);

// Network operations
int nuwan_socket_create(void);
int nuwan_socket_bind(int socket, int port);
int nuwan_socket_listen(int socket, int backlog);
int nuwan_socket_accept(int socket);
int nuwan_socket_connect(int socket, const char* host, int port);
int nuwan_socket_send(int socket, const char* data, int length);
int nuwan_socket_receive(int socket, char* buffer, int length);
int nuwan_socket_close(int socket);
char* nuwan_get_host_by_name(const char* hostname);

// GUI operations - See gui.h for the complete GUI API
// The old handle-based API has been deprecated in favor of the struct-based API in gui.h
// #include "gui.h" for the full GUI functionality

#ifdef __cplusplus
}
#endif

#endif // NUWAN_STDLIB_H

