/**
 * NnDataset API
 * 
 * Abstract interface for data providers
 */

#ifndef NN_DATASET_H
#define NN_DATASET_H

#include "../../core/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NnDataset interface */
typedef struct NnDataset {
    void* data;                    /* NnDataset-specific data */
    int num_samples;               /* Total number of samples */
    
    /* Get single sample */
    bool (*get_item)(struct NnDataset* self, int idx, Tensor** x, Tensor** y);
    
    /* Free dataset */
    void (*free_fn)(struct NnDataset* self);
} NnDataset;

/**
 * Create simple in-memory dataset
 * @param X Input data tensor [num_samples, ...]
 * @param y Target data tensor [num_samples, ...]
 */
NnDataset* nn_dataset_create_simple(Tensor* X, Tensor* y);

/**
 * Free dataset
 */
void nn_dataset_free(NnDataset* dataset);

#ifdef __cplusplus
}
#endif

#endif /* NN_DATASET_H */
