#ifndef ERROR_H
#define ERROR_H

#include "casprix/common.h"

void report_error(int line, int column, const char* message);
void report_error_at(int line, int column, const char* format, ...);
void report_type_error(int line, int column, const char* message);
void report_semantic_error(int line, int column, const char* message);

int get_error_count(void);
void reset_error_count(void);

extern bool had_error;
extern bool had_runtime_error;

#endif // ERROR_H