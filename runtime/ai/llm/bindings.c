/*
 * LLM Runtime - Casprix Language Bindings Implementation
 */

#include "bindings.h"
#include "training.h"
#include "backward.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ===== TOKENIZER BINDINGS =====

void* cpx_tokenizer_create(i32 vocab_size) {
    return (void*)tokenizer_create(vocab_size);
}

void cpx_tokenizer_train(void* handle, const char* text, i32 min_freq) {
    // For now, simplified training
    // Full implementation would call tokenizer_train
    Tokenizer* tok = (Tokenizer*)handle;
    (void)tok;
    // Training logic already in tokenizer_train
}

i32* cpx_tokenizer_encode(void* handle, const char* text, i32* out_len) {
    Tokenizer* tok = (Tokenizer*)handle;
    return tokenizer_encode(tok, text, out_len);
}

char* cpx_tokenizer_decode(void* handle, const i32* tokens, i32 len) {
    Tokenizer* tok = (Tokenizer*)handle;
    return tokenizer_decode(tok, tokens, len);
}

bool cpx_tokenizer_save(void* handle, const char* path) {
    Tokenizer* tok = (Tokenizer*)handle;
    return tokenizer_save(tok, path);
}

void* cpx_tokenizer_load(const char* path) {
    return (void*)tokenizer_load(path);
}

void cpx_tokenizer_destroy(void* handle) {
    Tokenizer* tok = (Tokenizer*)handle;
    tokenizer_destroy(tok);
}

// ===== DATASET BINDINGS =====

void* cpx_dataset_create(i32 num_seqs, i32 seq_len, i32 vocab_size) {
    return (void*)dataset_create(num_seqs, seq_len, vocab_size);
}

bool cpx_dataset_save(void* handle, const char* path) {
    Dataset* ds = (Dataset*)handle;
    return dataset_save(ds, path);
}

void* cpx_dataset_load(const char* path) {
    return (void*)dataset_load(path);
}

void cpx_dataset_shuffle(void* handle) {
    Dataset* ds = (Dataset*)handle;
    dataset_shuffle(ds);
}

const i32* cpx_dataset_get_sequence(void* handle, i32 index) {
    Dataset* ds = (Dataset*)handle;
    return dataset_get_sequence(ds, index);
}

i32 cpx_dataset_get_num_sequences(void* handle) {
    Dataset* ds = (Dataset*)handle;
    return ds->header.num_sequences;
}

i32 cpx_dataset_get_seq_length(void* handle) {
    Dataset* ds = (Dataset*)handle;
    return ds->header.seq_length;
}

void cpx_dataset_destroy(void* handle) {
    Dataset* ds = (Dataset*)handle;
    dataset_destroy(ds);
}

void** cpx_dataset_split(void* handle, f32 train_r, f32 val_r, f32 test_r) {
    Dataset* ds = (Dataset*)handle;
    Dataset** splits = dataset_split(ds, train_r, val_r, test_r);
    return (void**)splits;
}

// ===== MODEL BINDINGS =====

void* cpx_transformer_create(i32 vocab_size, i32 hidden_dim, i32 num_layers,
                                i32 num_heads, i32 ffn_dim, i32 max_seq_len) {
    return (void*)transformer_create(vocab_size, hidden_dim, num_layers,
                                      num_heads, ffn_dim, max_seq_len);
}

f32* cpx_transformer_forward(void* model, const i32* token_ids, 
                                i32 batch, i32 seq_len) {
    TransformerModel* m = (TransformerModel*)model;
    
    // Allocate memory pools
    MemoryPools* mem = mempool_create(0, 10*1024*1024, 5*1024*1024); // 10MB activations, 5MB grads
    
    // Allocate output logits
    i32 vocab_size = m->vocab_size;
    Tensor* logits = arena_alloc_tensor(mem->activations, 3, 
                                        (i32[]){batch, seq_len, vocab_size});
    
    // Forward pass
    transformer_forward(m, token_ids, batch, seq_len, logits, mem);
    
    // Copy logits to return array
    size_t logits_size = batch * seq_len * vocab_size;
    f32* result = (f32*)malloc(logits_size * sizeof(f32));
    memcpy(result, logits->data, logits_size * sizeof(f32));
    
    // Cleanup
    mempool_destroy(mem);
    
    return result;
}

bool cpx_transformer_save(void* model, const char* path) {
    // TODO: Implement checkpoint saving
    // Would save all weights to file
    return false;
}

void* cpx_transformer_load(const char* path) {
    // TODO: Implement checkpoint loading
    return NULL;
}

void cpx_transformer_destroy(void* model) {
    TransformerModel* m = (TransformerModel*)model;
    transformer_destroy(m);
}

// ===== TRAINING UTILITIES =====

f32 cpx_cross_entropy_loss(const f32* logits, const i32* targets,
                              i32 batch, i32 vocab_size) {
    return cross_entropy_loss(logits, targets, batch, vocab_size);
}

// ===== OPTIMIZER BINDINGS =====

void* cpx_adam_create(i32 num_params, f32 lr, f32 beta1, f32 beta2) {
    return (void*)adam_create(num_params, lr, beta1, beta2);
}

void cpx_adam_set_weight_decay(void* handle, f32 weight_decay) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    opt->weight_decay = weight_decay;
}

void cpx_adam_set_epsilon(void* handle, f32 epsilon) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    opt->epsilon = epsilon;
}

void cpx_adam_set_lr(void* handle, f32 lr) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    opt->learning_rate = lr;
}

f32 cpx_adam_get_lr(void* handle) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    return opt->learning_rate;
}

i32 cpx_adam_get_step(void* handle) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    return opt->t;
}

void cpx_adam_destroy(void* handle) {
    AdamOptimizer* opt = (AdamOptimizer*)handle;
    adam_destroy(opt);
}

// ===== TRAINING STEP BINDINGS =====

f32 cpx_train_step(void* model_handle, void* opt_handle, const i32* tokens,
                      const i32* targets, i32 batch, i32 seq_len) {
    TransformerModel* model = (TransformerModel*)model_handle;
    AdamOptimizer* opt = (AdamOptimizer*)opt_handle;

    // Allocate memory pools
    i32 vocab_size = model->vocab_size;
    i32 hidden_dim = model->hidden_dim;
    size_t act_size = (size_t)batch * seq_len * hidden_dim * 8;  // Generous activation space
    size_t grad_size = act_size;
    MemoryPools* mem = mempool_create(0, act_size, grad_size);

    // Allocate logits tensor
    Tensor* logits = arena_alloc_tensor(mem->activations, 3,
                                         (i32[]){batch, seq_len, vocab_size});

    // Forward pass
    transformer_forward(model, tokens, batch, seq_len, logits, mem);

    // Compute loss
    f32 loss = cross_entropy_loss(logits->data, targets, batch * seq_len, vocab_size);

    // Backward pass
    i32 num_params = opt->num_params;
    Tensor** param_grads = (Tensor**)calloc(num_params, sizeof(Tensor*));
    transformer_backward(model, logits, targets, batch, seq_len, param_grads, mem);

    // Collect parameter tensors from model
    // This is a simplified version - full implementation would enumerate all params
    Tensor** params = (Tensor**)calloc(num_params, sizeof(Tensor*));
    // TODO: Enumerate model parameters into params array

    // Optimizer step
    adam_step(opt, params, param_grads);

    // Cleanup
    free(params);
    free(param_grads);
    mempool_destroy(mem);

    return loss;
}

f32 cpx_eval_step(void* model_handle, const i32* tokens, const i32* targets,
                     i32 batch, i32 seq_len) {
    TransformerModel* model = (TransformerModel*)model_handle;
    i32 vocab_size = model->vocab_size;
    i32 hidden_dim = model->hidden_dim;

    size_t act_size = (size_t)batch * seq_len * hidden_dim * 4;
    MemoryPools* mem = mempool_create(0, act_size, 0);

    Tensor* logits = arena_alloc_tensor(mem->activations, 3,
                                         (i32[]){batch, seq_len, vocab_size});

    transformer_forward(model, tokens, batch, seq_len, logits, mem);

    f32 loss = cross_entropy_loss(logits->data, targets, batch * seq_len, vocab_size);

    mempool_destroy(mem);
    return loss;
}

// ===== LEARNING RATE SCHEDULE =====

f32 cpx_lr_schedule(i32 step, i32 warmup_steps, i32 max_steps,
                       f32 max_lr, f32 min_lr) {
    return lr_schedule_warmup_cosine(step, warmup_steps, max_steps, max_lr, min_lr);
}

// ===== DATA LOADER BINDINGS =====

void* cpx_dataloader_create(void* dataset, i32 batch_size, i32 shuffle) {
    Dataset* ds = (Dataset*)dataset;
    return (void*)dataloader_create(ds, batch_size, shuffle ? true : false);
}

i32 cpx_dataloader_next_batch(void* handle, i32** batch_out, i32** targets_out) {
    DataLoader* loader = (DataLoader*)handle;
    return dataloader_next_batch(loader, batch_out, targets_out) ? 1 : 0;
}

void cpx_dataloader_reset(void* handle) {
    DataLoader* loader = (DataLoader*)handle;
    dataloader_reset(loader);
}

void cpx_dataloader_destroy(void* handle) {
    DataLoader* loader = (DataLoader*)handle;
    dataloader_destroy(loader);
}

// ===== MODEL INFO BINDINGS =====

i32 cpx_transformer_get_num_params(void* model_handle) {
    TransformerModel* m = (TransformerModel*)model_handle;
    i32 hidden = m->hidden_dim;
    i32 vocab = m->vocab_size;
    i32 layers = m->num_layers;

    // Embeddings: token + position
    i32 emb_params = vocab * hidden + m->embeddings->max_seq_len * hidden;
    // Per layer: 4 attention weights + 2 FFN weights + 2 LN (gamma+beta each)
    i32 attn_params = 4 * hidden * hidden;
    i32 ffn_dim = m->blocks[0]->ffn->W1->shape[1];
    i32 ffn_params = hidden * ffn_dim + ffn_dim + ffn_dim * hidden + hidden;
    i32 ln_params = 2 * hidden * 2;  // 2 layernorms per block
    i32 per_layer = attn_params + ffn_params + ln_params;
    // Final LN + LM head
    i32 final_params = 2 * hidden + hidden * vocab;

    return emb_params + layers * per_layer + final_params;
}

i32 cpx_transformer_get_hidden_dim(void* model_handle) {
    TransformerModel* m = (TransformerModel*)model_handle;
    return m->hidden_dim;
}

i32 cpx_transformer_get_vocab_size(void* model_handle) {
    TransformerModel* m = (TransformerModel*)model_handle;
    return m->vocab_size;
}

i32 cpx_transformer_get_num_layers(void* model_handle) {
    TransformerModel* m = (TransformerModel*)model_handle;
    return m->num_layers;
}

// ===== GENERATION BINDINGS =====

i32* cpx_generate(void* model_handle, const i32* prompt_tokens, i32 prompt_len,
                     i32 max_new_tokens, f32 temperature, i32* out_len) {
    TransformerModel* model = (TransformerModel*)model_handle;
    i32 vocab_size = model->vocab_size;
    i32 hidden_dim = model->hidden_dim;
    i32 total_max = prompt_len + max_new_tokens;

    // Allocate output buffer
    i32* output = (i32*)malloc(total_max * sizeof(i32));
    memcpy(output, prompt_tokens, prompt_len * sizeof(i32));
    i32 cur_len = prompt_len;

    for (i32 step = 0; step < max_new_tokens; step++) {
        // Allocate memory for forward pass
        size_t act_size = (size_t)cur_len * hidden_dim * 4;
        MemoryPools* mem = mempool_create(0, act_size, 0);

        Tensor* logits = arena_alloc_tensor(mem->activations, 3,
                                             (i32[]){1, cur_len, vocab_size});

        // Forward pass
        transformer_forward(model, output, 1, cur_len, logits, mem);

        // Get logits for last position
        f32* last_logits = logits->data + (cur_len - 1) * vocab_size;

        i32 next_token;
        if (temperature <= 0.0f) {
            // Greedy: argmax
            next_token = 0;
            f32 max_val = last_logits[0];
            for (i32 i = 1; i < vocab_size; i++) {
                if (last_logits[i] > max_val) {
                    max_val = last_logits[i];
                    next_token = i;
                }
            }
        } else {
            // Temperature-scaled softmax sampling
            f32 max_val = last_logits[0];
            for (i32 i = 1; i < vocab_size; i++) {
                if (last_logits[i] > max_val) max_val = last_logits[i];
            }

            f32 sum = 0.0f;
            for (i32 i = 0; i < vocab_size; i++) {
                last_logits[i] = expf((last_logits[i] - max_val) / temperature);
                sum += last_logits[i];
            }

            // Random sampling
            f32 r = (f32)rand() / (f32)RAND_MAX * sum;
            f32 cumsum = 0.0f;
            next_token = vocab_size - 1;
            for (i32 i = 0; i < vocab_size; i++) {
                cumsum += last_logits[i];
                if (cumsum >= r) {
                    next_token = i;
                    break;
                }
            }
        }

        mempool_destroy(mem);

        output[cur_len] = next_token;
        cur_len++;

        // Stop at EOS (token 0 by convention)
        if (next_token == 0) break;
    }

    *out_len = cur_len;
    return output;
}

i32* cpx_generate_greedy(void* model_handle, const i32* prompt_tokens,
                            i32 prompt_len, i32 max_new_tokens, i32* out_len) {
    return cpx_generate(model_handle, prompt_tokens, prompt_len,
                           max_new_tokens, 0.0f, out_len);
}
