#ifndef CASPRIX_UTIL_PROCESS_H
#define CASPRIX_UTIL_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

/* Runs argv directly, without a shell. Returns the child exit code, or -1 if
 * process creation/waiting failed. Output is inherited unless quiet is true. */
int casprix_run_process(const char* const argv[], bool quiet);

/* Create a unique sibling path and atomically replace final_path after the
 * external tool succeeds. The temporary file is created as a reservation. */
bool casprix_reserve_sibling_temp(const char* final_path, char* temp_path,
                                 size_t temp_path_size);
bool casprix_publish_file(const char* temp_path, const char* final_path);

#endif
