#ifndef TOOLS_H
#define TOOLS_H

#include "casprix/common.h"
#include "support/log.h"

typedef enum {
    TOOL_NASM,
    TOOL_LD,
    TOOL_GCC  // For linking
} ToolType;

/* Logger parameter is deprecated - uses global nd_logger */
bool check_tool_available(ToolType tool, void* unused);
bool download_tool(ToolType tool, void* unused);
const char* get_tool_name(ToolType tool);
const char* get_tool_command(ToolType tool);

#endif // TOOLS_H



