/**
 * io.h - Source file and output artifact I/O utilities for the compiler driver
 */
#ifndef CASPRIX_DRIVER_IO_H
#define CASPRIX_DRIVER_IO_H

#include <stddef.h>
#include "driver/cli.h"

/**
 * Read an entire text file into a heap-allocated buffer.
 * Caller is responsible for free()ing the returned buffer.
 * Calls exit(74) on failure.
 */
char* driver_read_file(const char* path);

/**
 * Derive the base output name (without extension) from the input path,
 * respecting g_config.output_name if set.
 * Caller must free the returned string.
 */
char* driver_get_output_name(const char* input_path);

/**
 * Return the file extension for the current output kind (e.g. ".asm", ".c").
 * Points into a static string — do NOT free.
 */
const char* driver_get_output_extension(void);

/**
 * Concatenate output_name + extension into a heap string.
 * Caller must free.
 */
char* driver_get_output_file_path(const char* output_name);

/**
 * Human-readable label for the currently selected backend.
 */
const char* driver_selected_output_name(void);

#endif /* CASPRIX_DRIVER_IO_H */
