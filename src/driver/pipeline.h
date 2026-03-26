/**
 * pipeline.h - Compilation pipeline orchestration
 *
 * Declares the top-level compile and link entry points
 * so that main() stays minimal.
 */
#ifndef CASPRIX_DRIVER_PIPELINE_H
#define CASPRIX_DRIVER_PIPELINE_H

/**
 * Run the full compiler pipeline on source_path, emitting the intermediate
 * artifact to output_file_path (e.g. a .asm or .c file).
 *
 * @return 0 on success, non-zero on error.
 */
int pipeline_compile(const char* source_path, const char* output_file_path);

/**
 * Invoke NASM to assemble asm_file_path into obj_file_path.
 *
 * @return 0 on success, non-zero on error.
 */
int pipeline_assemble(const char* asm_file_path, const char* obj_file_path);

/**
 * Invoke GCC to link obj_file_path into a native executable at exe_path.
 * Cleans up intermediate files according to g_config.keep_asm_file.
 *
 * @return 0 on success, non-zero on error.
 */
int pipeline_link(const char* obj_file_path, const char* asm_file_path, const char* exe_path);

#endif /* CASPRIX_DRIVER_PIPELINE_H */
