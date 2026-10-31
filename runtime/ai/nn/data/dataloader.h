/**
 * NnDataLoader
 * 
 * Efficient batching and shuffling for training
 */

#ifndef NN_DATALOADER_H
#define NN_DATALOADER_H

#include "dataset.h"
#include "../../core/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    NnDataset* dataset;
    int batch_size;
    bool shuffle;
    bool drop_last;      /* Drop last incomplete batch */
    
    /* Internal state */
    int* indices;        /* Shuffled indices */
    int current_idx;     /* Current position */
    int num_batches;
} NnDataLoader;

/**
 * Create dataloader
 * @param dataset NnDataset to load from
 * @param batch_size Batch size
 * @param shuffle Whether to shuffle data
 * @param drop_last Drop incomplete last batch
 */
NnDataLoader* nn_dataloader_create(NnDataset* dataset, int batch_size,
                              bool shuffle, bool drop_last);

/**
 * Get next batch
 * @param batch_x Output input batch [batch_size, ...]
 * @param batch_y Output target batch [batch_size, ...]
 * @return true if batch retrieved, false if epoch ended
 */
bool nn_dataloader_next(NnDataLoader* loader, Tensor** batch_x, Tensor** batch_y);

/**
 * Reset dataloader (shuffle if enabled)
 */
void nn_dataloader_reset(NnDataLoader* loader);

/**
 * Get number of batches
 */
int nn_dataloader_num_batches(NnDataLoader* loader);

/**
 * Free dataloader
 */
void nn_dataloader_free(NnDataLoader* loader);

#ifdef __cplusplus
}
#endif

#endif /* NN_DATALOADER_H */
