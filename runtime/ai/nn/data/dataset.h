/**
 * Dataset API
 * 
 * Abstract interface for data providers
 */

#ifndef NN_DATASET_H
#define NN_DATASET_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dataset interface */
typedef struct Dataset {
    void* data;                    /* Dataset-specific data */
    int num_samples;               /* Total number of samples */
    
    /* Get single sample */
    bool (*get_item)(struct Dataset* self, int idx, Tensor** x, Tensor** y);
    
    /* Free dataset */
    void (*free_fn)(struct Dataset* self);
} Dataset;

/**
 * Create simple in-memory dataset
 * @param X Input data tensor [num_samples, ...]
 * @param y Target data tensor [num_samples, ...]
 */
Dataset* dataset_create_simple(Tensor* X, Tensor* y);

/**
 * Free dataset
 */
void dataset_free(Dataset* dataset);

#ifdef __cplusplus
}
#endif

#endif /* NN_DATASET_H */
