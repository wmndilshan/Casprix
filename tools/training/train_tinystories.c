/*
 * Casprix Compiler - TinyStories Training Script
 * Train a Small Language Model on TinyStories dataset
 *
 * Compile: gcc -O3 -mavx2 -mfma train_tinystories.c runtime/llm/*.c -Iruntime/llm -lm -o train
 * Run: ./train --train data/tinystories_train.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define get_time_ms() (GetTickCount64())
#else
#include <sys/time.h>
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}
#endif

#include "tensor.h"
#include "ops.h"
#include "transformer.h"
#include "tokenizer.h"
#include "training.h"
#include "backward.h"
#include "checkpoint.h"

/* ============================================================================
 * Configuration
 * ==========================================================================*/

typedef struct {
    /* Model architecture */
    int vocab_size;
    int hidden_dim;
    int num_layers;
    int num_heads;
    int ffn_dim;
    int max_seq_len;

    /* Training */
    int batch_size;
    int seq_length;
    float learning_rate;
    float weight_decay;
    int warmup_steps;
    int max_steps;
    float grad_clip;

    /* Logging */
    int log_interval;
    int eval_interval;
    int save_interval;

    /* Paths */
    char train_data[256];
    char val_data[256];
    char checkpoint_dir[256];
} TrainConfig;

static TrainConfig default_config(void) {
    TrainConfig cfg = {
        /* Model - default (overridden from dataset) */
        .vocab_size = 8192,
        .hidden_dim = 256,
        .num_layers = 6,
        .num_heads = 8,
        .ffn_dim = 1024,
        .max_seq_len = 256,

        /* Training */
        .batch_size = 4,
        .seq_length = 256,
        .learning_rate = 3e-4f,
        .weight_decay = 0.01f,
        .warmup_steps = 100,
        .max_steps = 1000,
        .grad_clip = 1.0f,

        /* Logging */
        .log_interval = 10,
        .eval_interval = 100,
        .save_interval = 500,

        /* Paths */
        .train_data = "data/tinystories_train.bin",
        .val_data = "data/tinystories_val.bin",
        .checkpoint_dir = "checkpoints"
    };
    return cfg;
}

/* ============================================================================
 * Training State
 * ==========================================================================*/

typedef struct {
    TransformerModel* model;
    AdamOptimizer* optimizer;
    MemoryPools* memory;

    /* Parameter tracking */
    Tensor** params;
    Tensor** grads;
    int num_params;

    /* Training state */
    int step;
    float train_loss;
    float val_loss;
    float best_val_loss;

    /* Timing */
    long long start_time;
    long long step_time;
    float tokens_per_sec;
} TrainState;

/* ============================================================================
 * Utility Functions
 * ==========================================================================*/

static void print_header(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("  CASPRIX COMPILER - TinyStories Language Model Training\n");
    printf("================================================================================\n");
    printf("\n");
}

static void print_config(TrainConfig* cfg) {
    printf("Configuration:\n");
    printf("  Model: %d layers, %d hidden, %d heads, %d vocab\n",
           cfg->num_layers, cfg->hidden_dim, cfg->num_heads, cfg->vocab_size);
    printf("  Training: batch=%d, seq_len=%d, lr=%.1e\n",
           cfg->batch_size, cfg->seq_length, cfg->learning_rate);
    printf("  Steps: %d (warmup: %d)\n", cfg->max_steps, cfg->warmup_steps);
    printf("\n");
}

static int count_parameters(TransformerModel* model) {
    int total = 0;

    /* Embeddings */
    total += model->embeddings->token_embedding->size;
    total += model->embeddings->pos_embedding->size;

    /* Transformer blocks */
    for (int i = 0; i < model->num_layers; i++) {
        TransformerBlock* block = model->blocks[i];

        /* Attention */
        total += block->attn->Wq->size;
        total += block->attn->Wk->size;
        total += block->attn->Wv->size;
        total += block->attn->Wo->size;

        /* FFN */
        total += block->ffn->W1->size;
        total += block->ffn->b1->size;
        total += block->ffn->W2->size;
        total += block->ffn->b2->size;

        /* LayerNorm */
        total += block->ln1_gamma->size * 2;
        total += block->ln2_gamma->size * 2;
    }

    /* Final LayerNorm and LM head */
    total += model->ln_final_gamma->size * 2;
    total += model->lm_head->size;

    return total;
}

/* ============================================================================
 * Parameter Collection
 * ==========================================================================*/

static void collect_parameters(TransformerModel* model, Tensor*** params, Tensor*** grads, int* num_params) {
    /* Count total parameters */
    int count = 0;

    /* Embeddings: 2 */
    count += 2;

    /* Per block: 4 attention + 4 FFN + 4 LayerNorm = 12 */
    count += model->num_layers * 12;

    /* Final: 2 LayerNorm + 1 LM head = 3 */
    count += 3;

    *num_params = count;
    *params = (Tensor**)calloc(count, sizeof(Tensor*));
    *grads = (Tensor**)calloc(count, sizeof(Tensor*));

    int idx = 0;

    /* Embeddings */
    (*params)[idx] = model->embeddings->token_embedding;
    (*grads)[idx] = tensor_create(model->embeddings->token_embedding->ndim,
                                   model->embeddings->token_embedding->shape);
    tensor_zeros((*grads)[idx]);
    idx++;

    (*params)[idx] = model->embeddings->pos_embedding;
    (*grads)[idx] = tensor_create(model->embeddings->pos_embedding->ndim,
                                   model->embeddings->pos_embedding->shape);
    tensor_zeros((*grads)[idx]);
    idx++;

    /* Transformer blocks */
    for (int i = 0; i < model->num_layers; i++) {
        TransformerBlock* block = model->blocks[i];

        /* Attention weights */
        Tensor* attn_params[] = {block->attn->Wq, block->attn->Wk,
                                  block->attn->Wv, block->attn->Wo};
        for (int j = 0; j < 4; j++) {
            (*params)[idx] = attn_params[j];
            (*grads)[idx] = tensor_create(attn_params[j]->ndim, attn_params[j]->shape);
            tensor_zeros((*grads)[idx]);
            idx++;
        }

        /* FFN weights */
        Tensor* ffn_params[] = {block->ffn->W1, block->ffn->b1,
                                 block->ffn->W2, block->ffn->b2};
        for (int j = 0; j < 4; j++) {
            (*params)[idx] = ffn_params[j];
            (*grads)[idx] = tensor_create(ffn_params[j]->ndim, ffn_params[j]->shape);
            tensor_zeros((*grads)[idx]);
            idx++;
        }

        /* LayerNorm */
        Tensor* ln_params[] = {block->ln1_gamma, block->ln1_beta,
                                block->ln2_gamma, block->ln2_beta};
        for (int j = 0; j < 4; j++) {
            (*params)[idx] = ln_params[j];
            (*grads)[idx] = tensor_create(ln_params[j]->ndim, ln_params[j]->shape);
            tensor_zeros((*grads)[idx]);
            idx++;
        }
    }

    /* Final LayerNorm */
    (*params)[idx] = model->ln_final_gamma;
    (*grads)[idx] = tensor_create(1, model->ln_final_gamma->shape);
    tensor_zeros((*grads)[idx]);
    idx++;

    (*params)[idx] = model->ln_final_beta;
    (*grads)[idx] = tensor_create(1, model->ln_final_beta->shape);
    tensor_zeros((*grads)[idx]);
    idx++;

    /* LM head */
    (*params)[idx] = model->lm_head;
    (*grads)[idx] = tensor_create(model->lm_head->ndim, model->lm_head->shape);
    tensor_zeros((*grads)[idx]);
}

/* ============================================================================
 * Loss Computation
 * ==========================================================================*/

static float compute_cross_entropy_loss(const Tensor* logits, const int* targets,
                                          int batch, int seq_len, int vocab_size) {
    float total_loss = 0.0f;
    int count = 0;

    for (int b = 0; b < batch; b++) {
        for (int s = 0; s < seq_len - 1; s++) {
            int idx = b * seq_len + s;
            int target = targets[idx];

            if (target < 0 || target >= vocab_size) continue;

            const float* logits_row = logits->data + idx * vocab_size;

            /* Compute log softmax */
            float max_logit = logits_row[0];
            for (int v = 1; v < vocab_size; v++) {
                if (logits_row[v] > max_logit) max_logit = logits_row[v];
            }

            float sum_exp = 0.0f;
            for (int v = 0; v < vocab_size; v++) {
                sum_exp += expf(logits_row[v] - max_logit);
            }

            float log_softmax = (logits_row[target] - max_logit) - logf(sum_exp);
            total_loss -= log_softmax;
            count++;
        }
    }

    return (count > 0) ? total_loss / count : 0.0f;
}

/* ============================================================================
 * Backward Pass (Full autograd via tape)
 * ==========================================================================*/

/* transformer_backward() in transformer_backward.c implements full reverse-mode
 * AD: records the forward pass on an autograd tape, then replays backward through
 * all operations using existing backward kernels from backward.c.
 * All gradients for all parameters are computed automatically. */

/* ============================================================================
 * Training Step
 * ==========================================================================*/

static float train_step(TrainState* state, TrainConfig* cfg,
                        int* input_batch, int* target_batch) {
    /* Forward pass */
    int batch = cfg->batch_size;
    int seq_len = cfg->seq_length;
    int vocab_size = cfg->vocab_size;

    Tensor* logits = arena_alloc_tensor(state->memory->activations, 3,
                                         (int[]){batch, seq_len, vocab_size});
    if (!logits) {
        fprintf(stderr, "ERROR: Failed to allocate logits tensor\n");
        return 0.0f;
    }

    transformer_forward(state->model, input_batch, batch, seq_len, logits, state->memory);

    /* Compute loss */
    float loss = compute_cross_entropy_loss(logits, target_batch, batch, seq_len, vocab_size);

    /* Full backward pass via autograd tape */
    transformer_backward(state->model, logits, target_batch, batch, seq_len,
                         state->grads, state->memory);

    /* Gradient clipping */
    for (int i = 0; i < state->num_params; i++) {
        if (state->grads[i]) {
#ifdef HAS_AVX2
            grad_clip_norm_avx2(state->grads[i]->data,
                                (int)state->grads[i]->size, cfg->grad_clip);
#else
            /* Scalar gradient clipping */
            float norm = 0.0f;
            for (int j = 0; j < (int)state->grads[i]->size; j++) {
                norm += state->grads[i]->data[j] * state->grads[i]->data[j];
            }
            norm = sqrtf(norm);
            if (norm > cfg->grad_clip) {
                float scale = cfg->grad_clip / norm;
                for (int j = 0; j < (int)state->grads[i]->size; j++) {
                    state->grads[i]->data[j] *= scale;
                }
            }
#endif
        }
    }

    /* Optimizer step */
    adam_step(state->optimizer, state->params, state->grads);

    /* Reset memory arenas */
    arena_reset(state->memory->activations);
    arena_reset(state->memory->grad_cache);

    return loss;
}

/* ============================================================================
 * Evaluation
 * ==========================================================================*/

static float evaluate(TrainState* state, TrainConfig* cfg, Dataset* val_data) {
    if (!val_data) return 0.0f;

    float total_loss = 0.0f;
    int num_batches = 0;
    int max_batches = 10;

    DataLoader* loader = dataloader_create(val_data, cfg->batch_size, false);

    int* input_batch = NULL;
    int* target_batch = NULL;

    while (dataloader_next_batch(loader, &input_batch, &target_batch) && num_batches < max_batches) {
        int batch = cfg->batch_size;
        int seq_len = cfg->seq_length;
        int vocab_size = cfg->vocab_size;

        Tensor* logits = arena_alloc_tensor(state->memory->activations, 3,
                                             (int[]){batch, seq_len, vocab_size});

        transformer_forward(state->model, input_batch, batch, seq_len, logits, state->memory);

        float loss = compute_cross_entropy_loss(logits, target_batch, batch, seq_len, vocab_size);
        total_loss += loss;
        num_batches++;

        free(input_batch);
        free(target_batch);
        arena_reset(state->memory->activations);
    }

    dataloader_destroy(loader);

    return (num_batches > 0) ? total_loss / num_batches : 0.0f;
}

/* ============================================================================
 * Main Training Loop
 * ==========================================================================*/

int main(int argc, char** argv) {
    print_header();

    /* Configuration */
    TrainConfig cfg = default_config();

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            cfg.max_steps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lr") == 0 && i + 1 < argc) {
            cfg.learning_rate = atof(argv[++i]);
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            cfg.batch_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc) {
            strncpy(cfg.train_data, argv[++i], sizeof(cfg.train_data) - 1);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --steps N      Training steps (default: %d)\n", cfg.max_steps);
            printf("  --lr RATE      Learning rate (default: %.1e)\n", cfg.learning_rate);
            printf("  --batch N      Batch size (default: %d)\n", cfg.batch_size);
            printf("  --train PATH   Training data path\n");
            return 0;
        }
    }

    /* Load dataset */
    printf("Loading dataset...\n");
    Dataset* train_data = dataset_load(cfg.train_data);
    if (!train_data) {
        printf("ERROR: Could not load training data from '%s'\n", cfg.train_data);
        printf("Please ensure the data file exists.\n");
        return 1;
    }
    printf("  Loaded %d sequences (seq_len=%d, vocab=%d)\n",
           train_data->header.num_sequences,
           train_data->header.seq_length,
           train_data->header.vocab_size);

    /* Override model config from dataset */
    cfg.vocab_size = train_data->header.vocab_size;
    cfg.seq_length = train_data->header.seq_length;

    Dataset* val_data = dataset_load(cfg.val_data);
    if (val_data) {
        printf("  Loaded %d validation sequences\n", val_data->header.num_sequences);
    }

    print_config(&cfg);

    /* Create model */
    printf("Creating model...\n");
    TransformerModel* model = transformer_create(
        cfg.vocab_size, cfg.hidden_dim, cfg.num_layers,
        cfg.num_heads, cfg.ffn_dim, cfg.max_seq_len
    );
    if (!model) {
        printf("ERROR: Failed to create model\n");
        return 1;
    }

    int num_params = count_parameters(model);
    printf("  Parameters: %d (%.2fM)\n", num_params, num_params / 1e6f);

    /* Initialize training state */
    TrainState state = {0};
    state.model = model;
    state.best_val_loss = 1e9f;
    state.start_time = get_time_ms();

    /* Collect parameters and create gradients */
    collect_parameters(model, &state.params, &state.grads, &state.num_params);

    /* Create optimizer */
    state.optimizer = adam_create(state.num_params, cfg.learning_rate, 0.9f, 0.999f);
    if (!state.optimizer) {
        printf("ERROR: Failed to create optimizer\n");
        return 1;
    }
    for (int i = 0; i < state.num_params; i++) {
        adam_init_param(state.optimizer, i, state.params[i]->shape, state.params[i]->ndim);
    }

    /* Create memory pools */
    state.memory = (MemoryPools*)malloc(sizeof(MemoryPools));
    if (!state.memory) {
        printf("ERROR: Failed to allocate MemoryPools\n");
        return 1;
    }
    memset(state.memory, 0, sizeof(MemoryPools));
    state.memory->activations = arena_create(256 * 1024 * 1024);
    state.memory->grad_cache = arena_create(64 * 1024 * 1024);

    if (!state.memory->activations || !state.memory->grad_cache) {
        printf("ERROR: Failed to create memory arenas\n");
        return 1;
    }

    /* Create data loader */
    DataLoader* loader = dataloader_create(train_data, cfg.batch_size, true);
    if (!loader) {
        printf("ERROR: Failed to create data loader\n");
        return 1;
    }

    /* Create checkpoint directory */
#ifdef _WIN32
    CreateDirectoryA(cfg.checkpoint_dir, NULL);
#else
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", cfg.checkpoint_dir);
    system(cmd);
#endif

    /* Training loop */
    printf("\n");
    printf("================================================================================\n");
    printf("  Starting Training\n");
    printf("================================================================================\n");
    printf("\n");
    printf("  Step   | Loss     | Val Loss | LR       | Tokens/s | Time\n");
    printf("---------|----------|----------|----------|----------|--------\n");

    int* input_batch = NULL;
    int* target_batch = NULL;
    float running_loss = 0.0f;
    int loss_count = 0;

    for (state.step = 1; state.step <= cfg.max_steps; state.step++) {
        /* Get next batch */
        if (!dataloader_next_batch(loader, &input_batch, &target_batch)) {
            dataloader_reset(loader);
            dataloader_next_batch(loader, &input_batch, &target_batch);
        }

        /* Update learning rate */
        float lr = lr_schedule_warmup_cosine(state.step, cfg.warmup_steps, cfg.max_steps,
                                              cfg.learning_rate, cfg.learning_rate * 0.1f);
        state.optimizer->learning_rate = lr;

        /* Training step */
        float loss = train_step(&state, &cfg, input_batch, target_batch);
        running_loss += loss;
        loss_count++;
        state.train_loss = loss;

        free(input_batch);
        free(target_batch);

        /* Logging */
        if (state.step % cfg.log_interval == 0) {
            float avg_loss = running_loss / loss_count;
            float elapsed_sec = (float)(get_time_ms() - state.start_time) / 1000.0f;
            int tokens_total = state.step * cfg.batch_size * cfg.seq_length;
            float tokens_per_sec = tokens_total / elapsed_sec;

            /* Evaluation */
            float val_loss = 0.0f;
            if (state.step % cfg.eval_interval == 0 && val_data) {
                val_loss = evaluate(&state, &cfg, val_data);
                state.val_loss = val_loss;

                if (val_loss < state.best_val_loss) {
                    state.best_val_loss = val_loss;
                    printf("  * New best validation loss!\n");
                }
            }

            int minutes = (int)(elapsed_sec / 60);
            int seconds = (int)elapsed_sec % 60;

            printf("  %6d | %8.4f | %8.4f | %.2e | %8.0f | %02d:%02d\n",
                   state.step, avg_loss, val_loss, lr, tokens_per_sec, minutes, seconds);

            running_loss = 0.0f;
            loss_count = 0;
        }

        /* Save checkpoint */
        if (state.step % cfg.save_interval == 0) {
            char checkpoint_path[512];
            snprintf(checkpoint_path, sizeof(checkpoint_path),
                     "%s/model_step_%d.bin", cfg.checkpoint_dir, state.step);

            CheckpointHeader header = {
                .magic = 0x4D4F4445,
                .version = 1,
                .vocab_size = cfg.vocab_size,
                .hidden_dim = cfg.hidden_dim,
                .num_layers = cfg.num_layers,
                .num_heads = cfg.num_heads,
                .ffn_dim = cfg.ffn_dim,
                .max_seq_len = cfg.max_seq_len,
                .training_step = state.step,
                .learning_rate = state.optimizer->learning_rate,
                .last_loss = running_loss / (loss_count > 0 ? loss_count : 1),
                .reserved = {0}
            };
            if (checkpoint_save(model, &header, checkpoint_path)) {
                printf("  Saved checkpoint: %s\n", checkpoint_path);
            }
        }
    }

    printf("\n");
    printf("================================================================================\n");
    printf("  Training Complete!\n");
    printf("================================================================================\n");
    printf("  Final loss: %.4f\n", state.train_loss);
    printf("  Best validation loss: %.4f\n", state.best_val_loss);

    /* Save final model */
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/model_final.bin", cfg.checkpoint_dir);
    CheckpointHeader final_header = {
        .magic = 0x4D4F4445,
        .version = 1,
        .vocab_size = cfg.vocab_size,
        .hidden_dim = cfg.hidden_dim,
        .num_layers = cfg.num_layers,
        .num_heads = cfg.num_heads,
        .ffn_dim = cfg.ffn_dim,
        .max_seq_len = cfg.max_seq_len,
        .training_step = state.step,
        .learning_rate = state.optimizer->learning_rate,
        .last_loss = state.train_loss,
        .reserved = {0}
    };
    if (checkpoint_save(model, &final_header, final_path)) {
        printf("  Saved final model: %s\n", final_path);
    }

    /* Cleanup */
    dataloader_destroy(loader);

    for (int i = 0; i < state.num_params; i++) {
        tensor_destroy(state.grads[i]);
    }
    free(state.params);
    free(state.grads);

    adam_destroy(state.optimizer);
    arena_destroy(state.memory->activations);
    arena_destroy(state.memory->grad_cache);
    free(state.memory);

    transformer_destroy(model);
    dataset_destroy(train_data);
    if (val_data) dataset_destroy(val_data);

    printf("\nDone.\n");
    return 0;
}
