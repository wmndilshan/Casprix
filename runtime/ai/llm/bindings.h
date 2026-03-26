/*
 * LLM Runtime - Casprix Language Bindings
 * 
 * C bindings for tokenizer, dataset, and model operations
 * accessible from Casprix language
 */

#ifndef LLM_BINDINGS_H
#define LLM_BINDINGS_H

#include "tokenizer.h"
#include "transformer.h"

// ===== TOKENIZER BINDINGS =====

// Create tokenizer
void* nuwan_tokenizer_create(i32 vocab_size);

// Train tokenizer
void nuwan_tokenizer_train(void* handle, const char* text, i32 min_freq);

// Encode text (returns array, caller must free)
i32* nuwan_tokenizer_encode(void* handle, const char* text, i32* out_len);

// Decode tokens (returns string, caller must free)
char* nuwan_tokenizer_decode(void* handle, const i32* tokens, i32 len);

// Save tokenizer
bool nuwan_tokenizer_save(void* handle, const char* path);

// Load tokenizer
void* nuwan_tokenizer_load(const char* path);

// Destroy tokenizer
void nuwan_tokenizer_destroy(void* handle);

// ===== DATASET BINDINGS =====

// Create dataset
void* nuwan_dataset_create(i32 num_seqs, i32 seq_len, i32 vocab_size);

// Save dataset
bool nuwan_dataset_save(void* handle, const char* path);

// Load dataset
void* nuwan_dataset_load(const char* path);

// Shuffle dataset
void nuwan_dataset_shuffle(void* handle);

// Get sequence
const i32* nuwan_dataset_get_sequence(void* handle, i32 index);

// Get dataset info
i32 nuwan_dataset_get_num_sequences(void* handle);
i32 nuwan_dataset_get_seq_length(void* handle);

// Destroy dataset
void nuwan_dataset_destroy(void* handle);

// Split dataset (returns array of 3 dataset pointers)
void** nuwan_dataset_split(void* handle, f32 train_r, f32 val_r, f32 test_r);

// ===== MODEL BINDINGS =====

// Create transformer model
void* nuwan_transformer_create(i32 vocab_size, i32 hidden_dim, i32 num_layers,
                                i32 num_heads, i32 ffn_dim, i32 max_seq_len);

// Forward pass (returns logits)
f32* nuwan_transformer_forward(void* model, const i32* token_ids, 
                                i32 batch, i32 seq_len);

// Save checkpoint
bool nuwan_transformer_save(void* model, const char* path);

// Load checkpoint
void* nuwan_transformer_load(const char* path);

// Destroy model
void nuwan_transformer_destroy(void* model);

// ===== TRAINING UTILITIES =====

// Compute cross-entropy loss
f32 nuwan_cross_entropy_loss(const f32* logits, const i32* targets,
                              i32 batch, i32 vocab_size);

// ===== OPTIMIZER BINDINGS =====

// Create Adam optimizer
void* nuwan_adam_create(i32 num_params, f32 lr, f32 beta1, f32 beta2);

// Set optimizer weight decay
void nuwan_adam_set_weight_decay(void* handle, f32 weight_decay);

// Set optimizer epsilon
void nuwan_adam_set_epsilon(void* handle, f32 epsilon);

// Set learning rate (for LR scheduling)
void nuwan_adam_set_lr(void* handle, f32 lr);

// Get current learning rate
f32 nuwan_adam_get_lr(void* handle);

// Get current timestep
i32 nuwan_adam_get_step(void* handle);

// Destroy optimizer
void nuwan_adam_destroy(void* handle);

// ===== TRAINING STEP BINDINGS =====

// Combined forward + backward + optimizer step
// Returns training loss for this batch
f32 nuwan_train_step(void* model, void* optimizer, const i32* tokens,
                      const i32* targets, i32 batch, i32 seq_len);

// Forward pass only with loss computation
f32 nuwan_eval_step(void* model, const i32* tokens, const i32* targets,
                     i32 batch, i32 seq_len);

// ===== LEARNING RATE SCHEDULE =====

// Warmup + cosine decay schedule
f32 nuwan_lr_schedule(i32 step, i32 warmup_steps, i32 max_steps,
                       f32 max_lr, f32 min_lr);

// ===== DATA LOADER BINDINGS =====

// Create data loader from dataset
void* nuwan_dataloader_create(void* dataset, i32 batch_size, i32 shuffle);

// Get next batch (returns false at epoch end)
// Fills batch_out and targets_out pointers
i32 nuwan_dataloader_next_batch(void* handle, i32** batch_out, i32** targets_out);

// Reset loader to beginning
void nuwan_dataloader_reset(void* handle);

// Destroy data loader
void nuwan_dataloader_destroy(void* handle);

// ===== MODEL INFO BINDINGS =====

// Get total parameter count
i32 nuwan_transformer_get_num_params(void* model);

// Get model hidden dimension
i32 nuwan_transformer_get_hidden_dim(void* model);

// Get model vocab size
i32 nuwan_transformer_get_vocab_size(void* model);

// Get model number of layers
i32 nuwan_transformer_get_num_layers(void* model);

// ===== GENERATION BINDINGS =====

// Generate tokens autoregressively
// Returns array of generated token IDs (caller must free)
i32* nuwan_generate(void* model, const i32* prompt_tokens, i32 prompt_len,
                     i32 max_new_tokens, f32 temperature, i32* out_len);

// Greedy decode (temperature=0)
i32* nuwan_generate_greedy(void* model, const i32* prompt_tokens,
                            i32 prompt_len, i32 max_new_tokens, i32* out_len);

#endif // LLM_BINDINGS_H
