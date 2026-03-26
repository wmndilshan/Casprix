/**
 * Dataset Implementation
 */

#include "dataset.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    Tensor* X;
    Tensor* y;
    int* sample_shapes_x;
    int* sample_shapes_y;
} SimpleDataset;

static bool simple_dataset_get_item(Dataset* self, int idx, Tensor** x, Tensor** y) {
    SimpleDataset* dataset = (SimpleDataset*)self->data;
    
    if (idx < 0 || idx >= self->num_samples) {
        return false;
    }
    
    /* For now, assume 2D tensors [num_samples, features] */
    int x_features = dataset->X->shape[1];
    int y_features = dataset->y->ndim > 1 ? dataset->y->shape[1] : 1;
    
    /* Create sample tensors */
    int x_shape[] = {1, x_features};
    int y_shape[] = {1, y_features};
    
    *x = tensor_create(2, x_shape);
    *y = tensor_create(dataset->y->ndim > 1 ? 2 : 1, y_shape);
    
    /* Copy data */
    memcpy((*x)->data, dataset->X->data + idx * x_features, x_features * sizeof(float));
    memcpy((*y)->data, dataset->y->data + idx * y_features, y_features * sizeof(float));
    
    return true;
}

static void simple_dataset_free(Dataset* self) {
    SimpleDataset* dataset = (SimpleDataset*)self->data;
    /* Note: X and y are owned by caller, don't free */
    free(dataset);
    free(self);
}

Dataset* dataset_create_simple(Tensor* X, Tensor* y) {
    Dataset* dataset = (Dataset*)calloc(1, sizeof(Dataset));
    SimpleDataset* simple = (SimpleDataset*)calloc(1, sizeof(SimpleDataset));
    
    simple->X = X;
    simple->y = y;
    
    dataset->data = simple;
    dataset->num_samples = X->shape[0];
    dataset->get_item = simple_dataset_get_item;
    dataset->free_fn = simple_dataset_free;
    
    return dataset;
}

void dataset_free(Dataset* dataset) {
    if (dataset && dataset->free_fn) {
        dataset->free_fn(dataset);
    }
}
