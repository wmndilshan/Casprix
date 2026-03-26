/**
 * DataLoader Implementation
 */

#include "dataloader.h"
#include <stdlib.h>
#include <string.h>

/* Fisher-Yates shuffle */
static void shuffle_indices(int* indices, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
}

DataLoader* dataloader_create(Dataset* dataset, int batch_size,
                              bool shuffle, bool drop_last) {
    DataLoader* loader = (DataLoader*)calloc(1, sizeof(DataLoader));
    
    loader->dataset = dataset;
    loader->batch_size = batch_size;
    loader->shuffle = shuffle;
    loader->drop_last = drop_last;
    loader->current_idx = 0;
    
    /* Create indices */
    int n = dataset->num_samples;
    loader->indices = (int*)malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        loader->indices[i] = i;
    }
    
    /* Shuffle if requested */
    if (shuffle) {
        shuffle_indices(loader->indices, n);
    }
    
    /* Calculate number of batches */
    loader->num_batches = n / batch_size;
    if (!drop_last && n % batch_size != 0) {
        loader->num_batches++;
    }
    
    return loader;
}

bool dataloader_next(DataLoader* loader, Tensor** batch_x, Tensor** batch_y) {
    int n = loader->dataset->num_samples;
    
    /* Check if epoch ended */
    if (loader->current_idx >= n) {
        return false;
    }
    
    /* Determine actual batch size */
    int actual_batch_size = loader->batch_size;
    if (loader->current_idx + actual_batch_size > n) {
        if (loader->drop_last) {
            return false;
        }
        actual_batch_size = n - loader->current_idx;
    }
    
    /* Get sample shapes from first item */
    Tensor* sample_x;
    Tensor* sample_y;
    int first_idx = loader->indices[loader->current_idx];
    loader->dataset->get_item(loader->dataset, first_idx, &sample_x, &sample_y);
    
    /* Create batch tensors */
    int batch_x_shape[MAX_TENSOR_DIM];
    int batch_y_shape[MAX_TENSOR_DIM];
    
    batch_x_shape[0] = actual_batch_size;
    batch_y_shape[0] = actual_batch_size;
    
    for (int i = 1; i < sample_x->ndim; i++) {
        batch_x_shape[i] = sample_x->shape[i];
    }
    for (int i = 1; i < sample_y->ndim; i++) {
        batch_y_shape[i] = sample_y->shape[i];
    }
    
    *batch_x = tensor_create(sample_x->ndim, batch_x_shape);
    *batch_y = tensor_create(sample_y->ndim, batch_y_shape);
    
    /* Fill batch with first sample */
    int sample_x_size = sample_x->size;
    int sample_y_size = sample_y->size;
    memcpy((*batch_x)->data, sample_x->data, sample_x_size * sizeof(float));
    memcpy((*batch_y)->data, sample_y->data, sample_y_size * sizeof(float));
    
    tensor_destroy(sample_x);
    tensor_destroy(sample_y);
    
    /* Fill remaining batch */
    for (int i = 1; i < actual_batch_size; i++) {
        int idx = loader->indices[loader->current_idx + i];
        loader->dataset->get_item(loader->dataset, idx, &sample_x, &sample_y);
        
        memcpy((*batch_x)->data + i * sample_x_size, sample_x->data, sample_x_size * sizeof(float));
        memcpy((*batch_y)->data + i * sample_y_size, sample_y->data, sample_y_size * sizeof(float));
        
        tensor_destroy(sample_x);
        tensor_destroy(sample_y);
    }
    
    loader->current_idx += actual_batch_size;
    return true;
}

void dataloader_reset(DataLoader* loader) {
    loader->current_idx = 0;
    
    if (loader->shuffle) {
        shuffle_indices(loader->indices, loader->dataset->num_samples);
    }
}

int dataloader_num_batches(DataLoader* loader) {
    return loader->num_batches;
}

void dataloader_free(DataLoader* loader) {
    if (!loader) return;
    
    free(loader->indices);
    free(loader);
}
