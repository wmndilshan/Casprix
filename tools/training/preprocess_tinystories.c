/*
 * TinyStories Preprocessing Utility (C implementation)
 * Tokenizes TinyStories text and creates binary dataset
 */

#include "runtime/llm/tokenizer.h"
#include "runtime/llm/tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FILE_SIZE (1024 * 1024 * 1024)  // 1GB max
#define CHUNK_SIZE 4096

// Read entire file into string
char* read_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size > MAX_FILE_SIZE) {
        fprintf(stderr, "File too large: %zu bytes\n", size);
        fclose(f);
        return NULL;
    }
    
    char* data = (char*)malloc(size + 1);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(data, 1, size, f);
    data[read] = '\0';
    *out_size = read;
    
    fclose(f);
    return data;
}

int main(int argc, char** argv) {
    printf("========================================\n");
    printf("  TinyStories Preprocessing\n");
    printf("========================================\n\n");
    
    // Configuration
    const char* input_path = "data/tinystories_sample.txt";
    const char* output_train = "data/tinystories_train.bin";
    const char* output_val = "data/tinystories_val.bin";
    int seq_length = 128;
    int vocab_size = 256;  // Byte-level
    
    if (argc > 1) {
        input_path = argv[1];
    }
    
    printf("Configuration:\n");
    printf("  Input: %s\n", input_path);
    printf("  Sequence length: %d\n", seq_length);
    printf("  Vocabulary: %d (byte-level)\n", vocab_size);
    printf("\n");
    
    // Read input file
    printf("[1/5] Reading input file...\n");
    size_t file_size;
    char* text = read_file(input_path, &file_size);
    if (!text) {
        return 1;
    }
    printf("✓ Loaded %zu bytes\n\n", file_size);
    
    // Tokenize (byte-level for now)
    printf("[2/5] Tokenizing text...\n");
    size_t num_tokens = file_size;
    printf("✓ Generated %zu tokens\n\n", num_tokens);
    
    // Create sequences
    printf("[3/5] Creating sequences...\n");
    int num_sequences = (num_tokens / seq_length);
    if (num_sequences == 0) {
        fprintf(stderr, "Error: Not enough tokens for even one sequence\n");
        free(text);
        return 1;
    }
    
    printf("✓ Created %d sequences\n\n", num_sequences);
    
    // Create dataset
    printf("[4/5] Building dataset...\n");
    Dataset* ds = dataset_create(num_sequences, seq_length, vocab_size);
    if (!ds) {
        fprintf(stderr, "Failed to create dataset\n");
        free(text);
        return 1;
    }
    
    // Fill dataset with byte-level tokens
    for (int i = 0; i < num_sequences * seq_length && i < file_size; i++) {
        ds->data[i] = (u8)text[i];  // Byte-level encoding
    }
    
    printf("✓ Dataset created\n\n");
    
    // Split into train/val
    printf("[5/5] Splitting and saving...\n");
    Dataset** splits = dataset_split(ds, 0.9f, 0.1f, 0.0f);
    
    if (splits) {
        if (!dataset_save(splits[0], output_train)) {
            fprintf(stderr, "Failed to save training set\n");
        } else {
            printf("✓ Training set: %d sequences -> %s\n", 
                   splits[0]->header.num_sequences, output_train);
        }
        
        if (!dataset_save(splits[1], output_val)) {
            fprintf(stderr, "Failed to save validation set\n");
        } else {
            printf("✓ Validation set: %d sequences -> %s\n",
                   splits[1]->header.num_sequences, output_val);
        }
        
        dataset_destroy(splits[0]);
        dataset_destroy(splits[1]);
        if (splits[2]) dataset_destroy(splits[2]);
        free(splits);
    }
    
    dataset_destroy(ds);
    free(text);
    
    printf("\n========================================\n");
    printf("  ✓ Preprocessing Complete!\n");
    printf("========================================\n");
    
    return 0;
}
