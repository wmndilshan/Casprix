/*
 * LLM Runtime - Training Utilities Implementation
 */

#include "training.h"
#include "ops.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

// ===== ADAM OPTIMIZER =====

AdamOptimizer* adam_create(i32 num_params, f32 lr, f32 beta1, f32 beta2) {
    AdamOptimizer* opt = (AdamOptimizer*)malloc(sizeof(AdamOptimizer));
    
    opt->learning_rate = lr;
    opt->beta1 = beta1;
    opt->beta2 = beta2;
    opt->epsilon = 1e-8f;
    opt->weight_decay = 0.01f;
    opt->t = 0;
    opt->num_params = num_params;
    
    // Allocate moment arrays
    opt->m = (Tensor**)calloc(num_params, sizeof(Tensor*));
    opt->v = (Tensor**)calloc(num_params, sizeof(Tensor*));
    
    return opt;
}

void adam_init_param(AdamOptimizer* opt, i32 param_idx, const i32* shape, i32 ndim) {
    opt->m[param_idx] = tensor_create(ndim, shape);
    opt->v[param_idx] = tensor_create(ndim, shape);
    
    tensor_zeros(opt->m[param_idx]);
    tensor_zeros(opt->v[param_idx]);
}

void adam_step(AdamOptimizer* opt, Tensor** params, Tensor** grads) {
    opt->t++;
    
    // Bias correction terms
    f32 bias_correction1 = 1.0f - powf(opt->beta1, (f32)opt->t);
    f32 bias_correction2 = 1.0f - powf(opt->beta2, (f32)opt->t);
    
    for (i32 i = 0; i < opt->num_params; i++) {
        if (!params[i] || !grads[i]) continue;
        
        Tensor* param = params[i];
        Tensor* grad = grads[i];
        Tensor* m = opt->m[i];
        Tensor* v = opt->v[i];
        
        // Update biased first moment: m = beta1 * m + (1 - beta1) * grad
        for (size_t j = 0; j < param->size; j++) {
            m->data[j] = opt->beta1 * m->data[j] + (1.0f - opt->beta1) * grad->data[j];
        }
        
        // Update biased second moment: v = beta2 * v + (1 - beta2) * grad^2
        for (size_t j = 0; j < param->size; j++) {
            v->data[j] = opt->beta2 * v->data[j] + (1.0f - opt->beta2) * grad->data[j] * grad->data[j];
        }
        
        // Update parameters
        for (size_t j = 0; j < param->size; j++) {
            f32 m_hat = m->data[j] / bias_correction1;
            f32 v_hat = v->data[j] / bias_correction2;
            
            // Weight decay (L2 regularization)
            f32 update = m_hat / (sqrtf(v_hat) + opt->epsilon);
            update += opt->weight_decay * param->data[j];
            
            param->data[j] -= opt->learning_rate * update;
        }
    }
}

void adam_destroy(AdamOptimizer* opt) {
    if (!opt) return;
    
    for (i32 i = 0; i < opt->num_params; i++) {
        tensor_destroy(opt->m[i]);
        tensor_destroy(opt->v[i]);
    }
    
    free(opt->m);
    free(opt->v);
    free(opt);
}

// ===== LEARNING RATE SCHEDULE =====

f32 lr_schedule_warmup_cosine(i32 step, i32 warmup_steps, i32 max_steps, 
                               f32 max_lr, f32 min_lr) {
    // Linear warmup
    if (step < warmup_steps) {
        return max_lr * ((f32)step / (f32)warmup_steps);
    }
    
    // Cosine decay
    f32 progress = (f32)(step - warmup_steps) / (f32)(max_steps - warmup_steps);
    f32 cosine = 0.5f * (1.0f + cosf(3.14159265359f * progress));
    return min_lr + (max_lr - min_lr) * cosine;
}

// ===== GRADIENT COMPUTATION =====
// transformer_backward() is implemented in transformer_backward.c using autograd tape

// ===== DATA LOADING =====

DataLoader* dataloader_create(Dataset* ds, i32 batch_size, bool shuffle) {
    DataLoader* loader = (DataLoader*)malloc(sizeof(DataLoader));
    
    loader->dataset = ds;
    loader->batch_size = batch_size;
    loader->seq_length = ds->header.seq_length;
    loader->current_idx = 0;
    loader->shuffle = shuffle;
    
    if (shuffle) {
        dataset_shuffle(ds);
    }
    
    return loader;
}

bool dataloader_next_batch(DataLoader* loader, i32** batch_out, i32** targets_out) {
    Dataset* ds = loader->dataset;
    i32 batch_size = loader->batch_size;
    i32 seq_len = loader->seq_length;
    
    // Check if we have enough sequences
    if (loader->current_idx + batch_size > ds->header.num_sequences) {
        return false;  // End of dataset
    }
    
    // Allocate batch arrays
    *batch_out = (i32*)malloc(batch_size * seq_len * sizeof(i32));
    *targets_out = (i32*)malloc(batch_size * seq_len * sizeof(i32));
    
    // Fill batch
    for (i32 b = 0; b < batch_size; b++) {
        const i32* sequence = dataset_get_sequence(ds, loader->current_idx + b);
        
        // Copy input (tokens 0 to seq_len-1)
        memcpy((*batch_out) + b * seq_len, sequence, seq_len * sizeof(i32));
        
        // Create targets (shifted by 1: tokens 1 to seq_len)
        for (i32 s = 0; s < seq_len - 1; s++) {
            (*targets_out)[b * seq_len + s] = sequence[s + 1];
        }
        // Last target is padding or next token
        (*targets_out)[b * seq_len + seq_len - 1] = 0;  // PAD token
    }
    
    loader->current_idx += batch_size;
    return true;
}

void dataloader_reset(DataLoader* loader) {
    loader->current_idx = 0;
    
    if (loader->shuffle) {
        dataset_shuffle(loader->dataset);
    }
}

void dataloader_destroy(DataLoader* loader) {
    free(loader);
}

// ===== GRADIENT CHECKPOINTING =====

/* Integer square root (floor). */
static i32 isqrt32(i32 n) {
    if (n <= 0) return 0;
    i32 x = (i32)sqrtf((f32)n);
    while ((x + 1) * (x + 1) <= n) x++;
    while (x * x > n) x--;
    return x;
}

GradCheckpointCtx* grad_checkpoint_create(const TransformerModel* model,
                                           MemoryPools* mem,
                                           i32 interval) {
    GradCheckpointCtx* ctx = (GradCheckpointCtx*)malloc(sizeof(GradCheckpointCtx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(GradCheckpointCtx));

    ctx->num_layers = model->num_layers;

    /* Default interval: ceil(sqrt(num_layers)) */
    if (interval <= 0) {
        i32 s = isqrt32(model->num_layers);
        interval = (s * s < model->num_layers) ? s + 1 : s;
        if (interval < 1) interval = 1;
    }
    ctx->interval = interval;

    /* Number of checkpoint saves = how many segment boundaries exist. */
    ctx->num_checkpoints = (model->num_layers + interval - 1) / interval;
    if (ctx->num_checkpoints > GRAD_CKPT_MAX_CHECKPOINTS)
        ctx->num_checkpoints = GRAD_CKPT_MAX_CHECKPOINTS;

    /* Pre-allocate checkpoint tensors in long-lived memory.
     * Shape will be filled in during forward; allocate with zeros for now.
     * Actual data written in transformer_forward_checkpointed. */
    (void)mem; /* mem used for scratch during recompute, not here */
    for (i32 i = 0; i < ctx->num_checkpoints; i++) {
        ctx->checkpoint_inputs[i] = NULL; /* allocated lazily on first forward */
    }

    return ctx;
}

void transformer_forward_checkpointed(TransformerModel* model,
                                       const i32* token_ids,
                                       i32 batch, i32 seq_len,
                                       Tensor* logits,
                                       GradCheckpointCtx* ctx,
                                       MemoryPools* mem) {
    i32 hidden_dim = model->hidden_dim;

    /* Embedding. */
    Tensor* x = tensor_arena_alloc_tensor(mem->activations, 3,
                                    (i32[]){batch, seq_len, hidden_dim});
    embeddings_forward(model->embeddings, token_ids, batch, seq_len, x);

    Tensor* block_out = tensor_arena_alloc_tensor(mem->activations, 3,
                                            (i32[]){batch, seq_len, hidden_dim});

    for (i32 i = 0; i < model->num_layers; i++) {
        /* Save checkpoint at start of each segment. */
        if (i % ctx->interval == 0) {
            i32 ckpt_idx = i / ctx->interval;
            if (ckpt_idx < ctx->num_checkpoints) {
                /* Allocate or reuse checkpoint tensor. */
                if (!ctx->checkpoint_inputs[ckpt_idx]) {
                    ctx->checkpoint_inputs[ckpt_idx] =
                        tensor_create(3, (i32[]){batch, seq_len, hidden_dim});
                }
                /* Copy current x into checkpoint. */
                tensor_copy(x, ctx->checkpoint_inputs[ckpt_idx]);
            }
        }

        /* Run the transformer block. */
        transformer_block_forward(model->blocks[i], x, block_out, mem->activations);

        /* Swap x ↔ block_out for next layer. */
        Tensor* tmp = x;
        x = block_out;
        block_out = tmp;
    }

    /* Final LayerNorm. */
    Tensor* ln_out = tensor_arena_alloc_tensor(mem->activations, 3,
                                         (i32[]){batch, seq_len, hidden_dim});
    layernorm_f32(x->data, model->ln_final_gamma->data, model->ln_final_beta->data,
                  ln_out->data, batch * seq_len, hidden_dim, 1e-5f);

    /* Project to vocab. */
    gemm_f32(ln_out->data, model->lm_head->data, logits->data,
             batch * seq_len, hidden_dim, model->vocab_size);
}

void grad_checkpoint_recompute(TransformerModel* model,
                                GradCheckpointCtx* ctx,
                                i32 layer_start, i32 layer_end,
                                Tensor* out,
                                MemoryPools* mem) {
    /* Find the checkpoint that covers layer_start. */
    i32 ckpt_idx = layer_start / ctx->interval;
    if (ckpt_idx >= ctx->num_checkpoints || !ctx->checkpoint_inputs[ckpt_idx]) {
        /* No checkpoint available — caller error. */
        return;
    }

    i32 hidden_dim = model->hidden_dim;
    Tensor* saved  = ctx->checkpoint_inputs[ckpt_idx];
    i32     batch  = saved->shape[0];
    i32     seq_len = saved->shape[1];

    /* Copy checkpoint input into a scratch activation tensor. */
    Tensor* x = tensor_arena_alloc_tensor(mem->activations, 3,
                                    (i32[]){batch, seq_len, hidden_dim});
    tensor_copy(saved, x);

    Tensor* tmp_out = tensor_arena_alloc_tensor(mem->activations, 3,
                                          (i32[]){batch, seq_len, hidden_dim});

    /* Re-run layers from layer_start up to (but not including) layer_end. */
    i32 end = layer_end < model->num_layers ? layer_end : model->num_layers;
    for (i32 i = layer_start; i < end; i++) {
        transformer_block_forward(model->blocks[i], x, tmp_out, mem->activations);
        Tensor* swap = x; x = tmp_out; tmp_out = swap;
    }

    /* Write result into caller-supplied out. */
    tensor_copy(x, out);
}

void grad_checkpoint_destroy(GradCheckpointCtx* ctx) {
    if (!ctx) return;
    for (i32 i = 0; i < ctx->num_checkpoints; i++) {
        tensor_destroy(ctx->checkpoint_inputs[i]);
    }
    free(ctx);
}
