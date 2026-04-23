/*
 * LLM Runtime - Auxiliary Utilities & Missing Bindings
 */

#include "bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ===== POINTER HELPERS =====

void* ptr_create_i32() {
    return calloc(1, sizeof(int32_t));
}

void ptr_free(void* ptr) {
    free(ptr);
}

int32_t ptr_read_i32(void* ptr) {
    if (!ptr) return 0;
    return *(int32_t*)ptr;
}

// ===== TIMING =====

int32_t get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ===== PRINTING =====

void print_f32(float val) {
    printf("%.4f\n", val);
}

void print_loss_log(int32_t step, float loss, float lr, int32_t dt_ms) {
    printf("[Step %d] loss: %.4f, lr: %.6f, dt: %dms\n", step, loss, lr, dt_ms);
}

// ===== CORPUS SHARDING =====

void cpx_encode_corpus_to_shards(const char* input_path, const char* output_prefix, 
                                   void* tokenizer_handle, int32_t shard_size) {
    Tokenizer* tok = (Tokenizer*)tokenizer_handle;
    if (!tok) {
        fprintf(stderr, "Error: Invalid tokenizer handle in cpx_encode_corpus_to_shards\n");
        return;
    }

    FILE* fin = fopen(input_path, "r");
    if (!fin) {
        fprintf(stderr, "Error: Could not open input file %s\n", input_path);
        return;
    }

    // Read the whole file
    fseek(fin, 0, SEEK_END);
    long size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    char* text = (char*)malloc(size + 1);
    if (!text) {
        fclose(fin);
        fprintf(stderr, "Error: Out of memory reading corpus\n");
        return;
    }
    size_t read_bytes = fread(text, 1, size, fin);
    text[read_bytes] = '\0';
    fclose(fin);

    // Tokenize
    int32_t num_tokens = 0;
    int32_t* tokens = tokenizer_encode(tok, text, &num_tokens);
    free(text);

    if (!tokens) {
        fprintf(stderr, "Error: Tokenization failed\n");
        return;
    }

    if (num_tokens == 0) {
        fprintf(stderr, "Warning: Corpus produced 0 tokens\n");
        free(tokens);
        return;
    }

    // Save shards
    int32_t num_shards = (num_tokens + shard_size - 1) / shard_size;
    printf("Sharding: %d tokens -> %d shards of size %d\n", num_tokens, num_shards, shard_size);

    for (int32_t i = 0; i < num_shards; i++) {
        char shard_path[1024];
        snprintf(shard_path, sizeof(shard_path), "%s_%d.bin", output_prefix, i);
        
        int32_t current_shard_size = shard_size;
        if (i == num_shards - 1) {
            current_shard_size = num_tokens - i * shard_size;
        }

        // Create a temporary dataset to save the shard
        // We use 1 sequence of length current_shard_size
        Dataset* ds = dataset_create(1, current_shard_size, tok->vocab->size);
        if (ds) {
            memcpy(ds->data, &tokens[i * shard_size], current_shard_size * sizeof(int32_t));
            dataset_save(ds, shard_path);
            dataset_destroy(ds);
        }
    }

    free(tokens);
}
