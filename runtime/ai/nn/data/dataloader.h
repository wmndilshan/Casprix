/**
 * DataLoader
 * 
 * Efficient batching and shuffling for training
 */

#ifndef NN_DATALOADER_H
#define NN_DATALOADER_H

#include "dataset.h"
#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Dataset* dataset;
    int batch_size;
    bool shuffle;
    bool drop_last;      /* Drop last incomplete batch */
    
    /* Internal state */
    int* indices;        /* Shuffled indices */
    int current_idx;     /* Current position */
    int num_batches;
} DataLoader;

/**
 * Create dataloader
 * @param dataset Dataset to load from
 * @param batch_size Batch size
 * @param shuffle Whether to shuffle data
 * @param drop_last Drop incomplete last batch
 */
DataLoader* dataloader_create(Dataset* dataset, int batch_size, 
                              bool shuffle, bool drop_last);

/**
 * Get next batch
 * @param batch_x Output input batch [batch_size, ...]
 * @param batch_y Output target batch [batch_size, ...]
 * @return true if batch retrieved, false if epoch ended
 */
bool dataloader_next(DataLoader* loader, Tensor** batch_x, Tensor** batch_y);

/**
 * Reset dataloader (shuffle if enabled)
 */
void dataloader_reset(DataLoader* loader);

/**
 * Get number of batches
 */
int dataloader_num_batches(DataLoader* loader);

/**
 * Free dataloader
 */
void dataloader_free(DataLoader* loader);

#ifdef __cplusplus
}
#endif

#endif /* NN_DATALOADER_H */
