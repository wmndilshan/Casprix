#include "util/process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

static char* windows_quote_arg(const char* arg) {
    size_t n = strlen(arg);
    char* quoted = (char*)malloc(n * 2 + 3);
    if (!quoted) return NULL;
    char* out = quoted;
    *out++ = '"';
    size_t slashes = 0;
    for (const char* p = arg; ; ++p) {
        if (*p == '\\') {
            ++slashes;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; ++i) *out++ = '\\';
            *out++ = '"';
            slashes = 0;
            continue;
        }
        if (*p == '\0') {
            for (size_t i = 0; i < slashes * 2; ++i) *out++ = '\\';
            break;
        }
        for (size_t i = 0; i < slashes; ++i) *out++ = '\\';
        slashes = 0;
        *out++ = *p;
    }
    *out++ = '"';
    *out = '\0';
    return quoted;
}

int casprix_run_process(const char* const argv[], bool quiet) {
    if (!argv || !argv[0] || !argv[0][0]) return -1;
#ifdef _WIN32
    size_t capacity = 1;
    for (size_t i = 0; argv[i]; ++i) capacity += strlen(argv[i]) * 2 + 4;
    char* command_line = (char*)malloc(capacity);
    if (!command_line) return -1;
    command_line[0] = '\0';
    for (size_t i = 0; argv[i]; ++i) {
        char* q = windows_quote_arg(argv[i]);
        if (!q) { free(command_line); return -1; }
        if (i) strcat(command_line, " ");
        strcat(command_line, q);
        free(q);
    }

    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (quiet) {
        SECURITY_ATTRIBUTES null_security = { sizeof(null_security), NULL, TRUE };
        HANDLE null_handle = CreateFileA("NUL", GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &null_security, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (null_handle != INVALID_HANDLE_VALUE) {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = null_handle;
            startup.hStdError = null_handle;
        }
    } else {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    BOOL created = CreateProcessA(NULL, command_line, NULL, NULL, TRUE, 0,
                                  NULL, NULL, &startup, &process);
    free(command_line);
    if (!created) {
        if (quiet && startup.hStdOutput && startup.hStdOutput != INVALID_HANDLE_VALUE)
            CloseHandle(startup.hStdOutput);
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (quiet && startup.hStdOutput && startup.hStdOutput != INVALID_HANDLE_VALUE)
        CloseHandle(startup.hStdOutput);
    return (int)exit_code;
#else
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        if (quiet) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        execvp(argv[0], (char* const*)argv);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

bool casprix_reserve_sibling_temp(const char* final_path, char* temp_path,
                                 size_t temp_path_size) {
    if (!final_path || !temp_path || !temp_path_size) return false;
#ifdef _WIN32
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const char* slash1 = strrchr(final_path, '/');
        const char* slash2 = strrchr(final_path, '\\');
        const char* leaf = final_path;
        if (slash1 && slash1 + 1 > leaf) leaf = slash1 + 1;
        if (slash2 && slash2 + 1 > leaf) leaf = slash2 + 1;
        const char* dot = strrchr(leaf, '.');
        if (dot == leaf) dot = NULL;
        size_t stem = dot ? (size_t)(dot - final_path) : strlen(final_path);
        const char* extension = dot ? dot : "";
        int n = snprintf(temp_path, temp_path_size, "%.*s.casprix-tmp-%ld-%u%s",
                         (int)stem, final_path, pid, attempt, extension);
        if (n < 0 || (size_t)n >= temp_path_size) return false;
#ifdef _WIN32
        int fd = _open(temp_path, _O_CREAT | _O_EXCL | _O_BINARY | _O_WRONLY,
                       _S_IREAD | _S_IWRITE);
        if (fd >= 0) { _close(fd); return true; }
#else
        int fd = open(temp_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) { close(fd); return true; }
#endif
    }
    return false;
}

bool casprix_publish_file(const char* temp_path, const char* final_path) {
#ifdef _WIN32
    return MoveFileExA(temp_path, final_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temp_path, final_path) == 0;
#endif
}
