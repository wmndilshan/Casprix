/*
 * LLM Runtime - BPE Tokenizer
 * 
 * Fast Byte Pair Encoding tokenizer for LLM training
 */

#ifndef LLM_TOKENIZER_H
#define LLM_TOKENIZER_H

#include "tensor.h"

// ===== TOKENIZER =====

typedef struct TokenizerVocab {
    char** tokens;        // Token strings
    i32*   ids;           // Token IDs
    i32    size;          // Vocabulary size
} TokenizerVocab;

typedef struct BPEMerge {
    i32 token_a;
    i32 token_b;
    i32 merged_token;
    i32 freq;            // Frequency (for training)
} BPEMerge;

typedef struct Tokenizer {
    TokenizerVocab* vocab;
    BPEMerge*  merges;
    i32        num_merges;
    i32        max_token_len;
    
    // Special tokens
    i32 bos_token;       // Beginning of sequence
    i32 eos_token;       // End of sequence
    i32 pad_token;       // Padding
    i32 unk_token;       // Unknown
} Tokenizer;

// Create empty tokenizer
Tokenizer* tokenizer_create(i32 vocab_size);

// Train BPE tokenizer on text corpus
// text: input text
// vocab_size: desired vocabulary size
// min_freq: minimum frequency for a merge
Tokenizer* tokenizer_train(const char* text, i32 vocab_size, i32 min_freq);

// Encode text to token IDs
// Returns array of token IDs (caller must free)
// out_len: number of tokens
i32* tokenizer_encode(Tokenizer* tok, const char* text, i32* out_len);

// Decode token IDs back to text
// Returns string (caller must free)
char* tokenizer_decode(Tokenizer* tok, const i32* tokens, i32 len);

// Save tokenizer to file
bool tokenizer_save(Tokenizer* tok, const char* path);

// Load tokenizer from file
Tokenizer* tokenizer_load(const char* path);

// Destroy tokenizer
void tokenizer_destroy(Tokenizer* tok);

// ===== BINARY DATA FORMAT =====

typedef struct DatasetHeader {
    u32 magic;           // 0x54494E59 ("TINY")
    u32 version;         // Format version
    i32 num_sequences;   // Total sequences
    i32 seq_length;      // Sequence length
    i32 vocab_size;      // Vocabulary size
    i32 reserved[3];     // Reserved for future use
} DatasetHeader;

typedef struct Dataset {
    DatasetHeader header;
    i32* data;           // Flattened token array [num_sequences * seq_length]
    size_t data_size;    // Size in bytes
    bool owns_memory;    // If true, free data on destroy
} Dataset;

// Create dataset
Dataset* dataset_create(i32 num_sequences, i32 seq_length, i32 vocab_size);

// Save dataset to binary file
bool dataset_save(Dataset* ds, const char* path);

// Load dataset from binary file
Dataset* dataset_load(const char* path);

// Memory-map dataset (for efficient loading during training)
Dataset* dataset_mmap(const char* path);

// Get sequence at index
// Returns pointer to data (do not free)
const i32* dataset_get_sequence(Dataset* ds, i32 index);

// Destroy dataset
void dataset_destroy(Dataset* ds);

// ===== UTILITIES =====

// Split dataset into train/val/test
// input: source dataset
// train_ratio, val_ratio, test_ratio: split ratios (must sum to 1.0)
// Returns array of 3 datasets [train, val, test] (caller must free)
Dataset** dataset_split(Dataset* input, f32 train_ratio, f32 val_ratio, f32 test_ratio);

// Shuffle dataset in-place
void dataset_shuffle(Dataset* ds);

#endif // LLM_TOKENIZER_H
