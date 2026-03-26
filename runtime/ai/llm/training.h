/*
 * LLM Runtime - Training Utilities
 * 
 * Adam optimizer and training loop utilities
 */

#ifndef LLM_TRAINING_H
#define LLM_TRAINING_H

#include "tensor.h"
#include "transformer.h"
#include "tokenizer.h"

// ===== ADAM OPTIMIZER =====

typedef struct AdamOptimizer {
    f32 learning_rate;
    f32 beta1;           // First moment decay
    f32 beta2;           // Second moment decay
    f32 epsilon;         // Numerical stability
    f32 weight_decay;    // L2 regularization
    i32 t;               // Timestep
    
    // Optimizer states (one per parameter)
    Tensor** m;          // First moment estimates
    Tensor** v;          // Second moment estimates
    i32 num_params;
} AdamOptimizer;

// Create Adam optimizer
AdamOptimizer* adam_create(i32 num_params, f32 lr, f32 beta1, f32 beta2);

// Initialize optimizer state for parameter
void adam_init_param(AdamOptimizer* opt, i32 param_idx, const i32* shape, i32 ndim);

// Optimizer step
// params: array of parameter tensors
// grads: array of gradient tensors
void adam_step(AdamOptimizer* opt, Tensor** params, Tensor** grads);

// Destroy optimizer
void adam_destroy(AdamOptimizer* opt);

// ===== LEARNING RATE SCHEDULE =====

// Linear warmup + cosine decay
f32 lr_schedule_warmup_cosine(i32 step, i32 warmup_steps, i32 max_steps, 
                               f32 max_lr, f32 min_lr);

// ===== GRADIENT COMPUTATION =====

// Full backward pass using autograd tape.
// Re-runs forward on tape, computes all gradients via reverse-mode AD.
// param_grads ordering matches collect_parameters() in train_tinystories.c.
void transformer_backward(TransformerModel* model, const Tensor* logits,
                          const i32* targets, i32 batch, i32 seq_len,
                          Tensor** param_grads, MemoryPools* mem);

// ===== DATA LOADING =====

typedef struct DataLoader {
    Dataset* dataset;
    i32 batch_size;
    i32 seq_length;
    i32 current_idx;
    bool shuffle;
} DataLoader;

// Create data loader
DataLoader* dataloader_create(Dataset* ds, i32 batch_size, bool shuffle);

// Get next batch
// Returns batch of sequences: [batch_size, seq_length]
// Also fills targets: [batch_size, seq_length] (shifted by 1)
bool dataloader_next_batch(DataLoader* loader, i32** batch_out, i32** targets_out);

// Reset to beginning
void dataloader_reset(DataLoader* loader);

// Destroy data loader
void dataloader_destroy(DataLoader* loader);

#endif // LLM_TRAINING_H
