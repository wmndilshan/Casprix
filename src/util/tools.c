/*
 * Casprix Compiler - Build Tools Utilities
 */

#include "util/tools.h"
#include "support/log.h"
#include <stdlib.h>
#include <stdio.h>

static const char* tool_names[] = {
    "nasm",
    "ld",
    "gcc"
};

static const char* tool_commands[] = {
#ifdef _WIN32
    "C:\\Program Files\\NASM\\nasm.exe",
    "ld.exe",
    "gcc.exe"
#else
    "nasm",
    "ld",
    "gcc"
#endif
};

const char* get_tool_name(ToolType tool) {
    if (tool >= 0 && tool < 3) {
        return tool_names[tool];
    }
    return "unknown";
}

const char* get_tool_command(ToolType tool) {
    if (tool >= 0 && tool < 3) {
        return tool_commands[tool];
    }
    return "unknown";
}

bool check_tool_available(ToolType tool, void* unused) {
    (void)unused;  // Deprecated parameter

    const char* cmd = get_tool_command(tool);
    char command[256];

#ifdef _WIN32
    snprintf(command, sizeof(command), "where %s >nul 2>&1", cmd);
#else
    snprintf(command, sizeof(command), "which %s >/dev/null 2>&1", cmd);
#endif

    int result = system(command);

    if (result == 0) {
        CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_GENERAL, "Tool '%s' is available", get_tool_name(tool));
        return true;
    } else {
        CPX_LOG(CPX_LOG_WARN,  CPX_LOG_CAT_GENERAL, "Tool '%s' not found in PATH", get_tool_name(tool));
        return false;
    }
}

bool download_tool(ToolType tool, void* unused) {
    (void)unused;  // Deprecated parameter

    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "Attempting to download %s...", get_tool_name(tool));

    switch (tool) {
        case TOOL_NASM:
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "Please download NASM from: https://www.nasm.us/");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "Or install via package manager:");
#ifdef _WIN32
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Using Chocolatey: choco install nasm");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Using Scoop: scoop install nasm");
#else
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Ubuntu/Debian: sudo apt-get install nasm");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Fedora: sudo dnf install nasm");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - macOS: brew install nasm");
#endif
            return false;

        case TOOL_LD:
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "LD linker is usually included with GCC/MinGW");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "Please install GCC/MinGW development tools");
            return false;

        case TOOL_GCC:
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "Please install GCC:");
#ifdef _WIN32
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - MinGW-w64: https://www.mingw-w64.org/");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Using Chocolatey: choco install mingw");
#else
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Ubuntu/Debian: sudo apt-get install gcc");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - Fedora: sudo dnf install gcc");
            CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_GENERAL, "  - macOS: xcode-select --install");
#endif
            return false;
    }

    return false;
}
