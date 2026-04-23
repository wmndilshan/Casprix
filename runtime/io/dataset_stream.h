#ifndef CASPRIX_DATASET_STREAM_H
#define CASPRIX_DATASET_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../ai/ml/cpx_mem_arena.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* base;
    size_t         len;
    size_t         cursor;
#ifdef _WIN32
    HANDLE         file_handle;
    HANDLE         mapping_handle;
#else
    int            fd;
#endif
} CpxMappedDataset;

int  cpx_dataset_map_readonly(const char* path, CpxMappedDataset* out);
void cpx_dataset_unmap(CpxMappedDataset* ds);

/* Safe bounded read from mapped data to arena memory. */
bool cpx_dataset_copy_to_arena(CpxMappedDataset* ds, size_t offset, size_t nbytes,
                               CpxArena* arena, size_t align, void** out_ptr);

/* Zero-copy view into mapped data with strict bounds check. */
bool cpx_dataset_view(const CpxMappedDataset* ds, size_t offset, size_t nbytes, const void** out_ptr);

/* Advise kernel on access pattern (e.g., sequential) to trigger read-ahead. */
int  cpx_dataset_advise(CpxMappedDataset* ds, size_t offset, size_t nbytes);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_DATASET_STREAM_H */
