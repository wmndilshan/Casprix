/*
 * LLM Runtime - BPE Tokenizer Implementation
 */

#include "tokenizer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

#include <ctype.h>

// ===== BYTE-LEVEL VOCABULARY =====

static void init_byte_vocab(TokenizerVocab* vocab) {
    // Initialize with 256 byte-level tokens
    vocab->size = 256;
    vocab->tokens = (char**)malloc(256 * sizeof(char*));
    vocab->ids = (i32*)malloc(256 * sizeof(i32));
    
    for (i32 i = 0; i < 256; i++) {
        vocab->tokens[i] = (char*)malloc(2);
        vocab->tokens[i][0] = (char)i;
        vocab->tokens[i][1] = '\0';
        vocab->ids[i] = i;
    }
}

// ===== TOKENIZER =====

Tokenizer* tokenizer_create(i32 vocab_size) {
    Tokenizer* tok = (Tokenizer*)malloc(sizeof(Tokenizer));
    
    tok->vocab = (TokenizerVocab*)malloc(sizeof(TokenizerVocab));
    init_byte_vocab(tok->vocab);
    
    tok->num_merges = vocab_size - 256;  // Number of merges to learn
    tok->merges = (BPEMerge*)calloc(tok->num_merges, sizeof(BPEMerge));
    tok->max_token_len = 16;
    
    // Special tokens (add to vocab)
    tok->bos_token = 0;   // <BOS>
    tok->eos_token = 1;   // <EOS>
    tok->pad_token = 2;   // <PAD>
    tok->unk_token = 3;   // <UNK>
    
    return tok;
}

// Simplified BPE training (full implementation would use frequency counting)
Tokenizer* tokenizer_train(const char* text, i32 vocab_size, i32 min_freq) {
    Tokenizer* tok = tokenizer_create(vocab_size);
    
    // TODO: Full BPE implementation:
    // 1. Count all byte pairs
    // 2. Find most frequent pair
    // 3. Merge pair into new token
    // 4. Repeat until vocab_size reached
    
    // For now, use simple heuristic: common bigrams
    const char* common_pairs[] = {
        "th", "he", "in", "er", "an", "re", "on", "at", "en", "nd",
        "ti", "es", "or", "te", "of", "ed", "is", "it", "al", "ar"
    };
    
    i32 num_common = 20;
    for (i32 i = 0; i < num_common && i < tok->num_merges; i++) {
        tok->merges[i].token_a = (i32)common_pairs[i][0];
        tok->merges[i].token_b = (i32)common_pairs[i][1];
        tok->merges[i].merged_token = 256 + i;
    }
    
    return tok;
}

// Simple greedy encoding
i32* tokenizer_encode(Tokenizer* tok, const char* text, i32* out_len) {
    if (!text || !out_len) return NULL;
    
    size_t text_len = strlen(text);
    
    // Allocate max possible tokens (one per character)
    i32* tokens = (i32*)malloc(text_len * sizeof(i32));
    i32 num_tokens = 0;
    
    // Byte-level encoding (simplified)
    for (size_t i = 0; i < text_len; i++) {
        tokens[num_tokens++] = (i32)(unsigned char)text[i];
    }
    
    // Apply merges (simplified - real BPE would iterate)
    // For now, just use byte-level tokens
    
    *out_len = num_tokens;
    return tokens;
}

// Decode tokens back to text
char* tokenizer_decode(Tokenizer* tok, const i32* tokens, i32 len) {
    if (!tokens || len <= 0) return NULL;
    
    // Allocate buffer (max: len * max_token_len)
    char* text = (char*)malloc((len * tok->max_token_len + 1) * sizeof(char));
    i32 pos = 0;
    
    for (i32 i = 0; i < len; i++) {
        i32 token_id = tokens[i];
        
        // Byte-level token
        if (token_id < 256) {
            text[pos++] = (char)token_id;
        }
        // Special tokens
        else if (token_id == tok->bos_token || token_id == tok->eos_token ||
                 token_id == tok->pad_token) {
            continue;  // Skip special tokens
        }
        // Merged tokens (would need vocab lookup)
        else {
            text[pos++] = '?';  // Unknown for now
        }
    }
    
    text[pos] = '\0';
    return text;
}

// Save tokenizer
bool tokenizer_save(Tokenizer* tok, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    
    // Write header
    fwrite(&tok->vocab->size, sizeof(i32), 1, f);
    fwrite(&tok->num_merges, sizeof(i32), 1, f);
    
    // Write merges
    fwrite(tok->merges, sizeof(BPEMerge), tok->num_merges, f);
    
    // Write special tokens
    fwrite(&tok->bos_token, sizeof(i32), 1, f);
    fwrite(&tok->eos_token, sizeof(i32), 1, f);
    fwrite(&tok->pad_token, sizeof(i32), 1, f);
    fwrite(&tok->unk_token, sizeof(i32), 1, f);
    
    fclose(f);
    return true;
}

// Load tokenizer
Tokenizer* tokenizer_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    
    Tokenizer* tok = (Tokenizer*)malloc(sizeof(Tokenizer));
    tok->vocab = (TokenizerVocab*)malloc(sizeof(TokenizerVocab));
    
    // Read header
    fread(&tok->vocab->size, sizeof(i32), 1, f);
    fread(&tok->num_merges, sizeof(i32), 1, f);
    
    // Initialize byte vocab
    init_byte_vocab(tok->vocab);
    
    // Read merges
    tok->merges = (BPEMerge*)malloc(tok->num_merges * sizeof(BPEMerge));
    fread(tok->merges, sizeof(BPEMerge), tok->num_merges, f);
    
    size_t ign;
    ign = fread(&tok->bos_token, sizeof(i32), 1, f);
    ign = fread(&tok->eos_token, sizeof(i32), 1, f);
    ign = fread(&tok->pad_token, sizeof(i32), 1, f);
    ign = fread(&tok->unk_token, sizeof(i32), 1, f);
    (void)ign;
    
    tok->max_token_len = 16;
    
    fclose(f);
    return tok;
}

void tokenizer_destroy(Tokenizer* tok) {
    if (!tok) return;
    
    if (tok->vocab) {
        for (i32 i = 0; i < tok->vocab->size; i++) {
            free(tok->vocab->tokens[i]);
        }
        free(tok->vocab->tokens);
        free(tok->vocab->ids);
        free(tok->vocab);
    }
    
    free(tok->merges);
    free(tok);
}

// ===== DATASET =====

#define DATASET_MAGIC 0x54494E59  // "TINY"

Dataset* dataset_create(i32 num_sequences, i32 seq_length, i32 vocab_size) {
    Dataset* ds = (Dataset*)malloc(sizeof(Dataset));
    
    ds->header.magic = DATASET_MAGIC;
    ds->header.version = 1;
    ds->header.num_sequences = num_sequences;
    ds->header.seq_length = seq_length;
    ds->header.vocab_size = vocab_size;
    
    ds->data_size = num_sequences * seq_length * sizeof(i32);
    ds->data = (i32*)aligned_alloc(64, ds->data_size);
    ds->owns_memory = true;
    
    return ds;
}

bool dataset_save(Dataset* ds, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    
    // Write header
    fwrite(&ds->header, sizeof(DatasetHeader), 1, f);
    
    // Write data
    fwrite(ds->data, sizeof(i32), 
           ds->header.num_sequences * ds->header.seq_length, f);
    
    fclose(f);
    return true;
}

Dataset* dataset_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    
    Dataset* ds = (Dataset*)malloc(sizeof(Dataset));
    
    // Read header
    size_t ign = fread(&ds->header, sizeof(DatasetHeader), 1, f);
    (void)ign;
    
    // Validate magic
    if (ds->header.magic != DATASET_MAGIC) {
        free(ds);
        fclose(f);
        return NULL;
    }
    
    // Read data
    ds->data_size = ds->header.num_sequences * ds->header.seq_length * sizeof(i32);
    ds->data = (i32*)aligned_alloc(64, ds->data_size);
    ign = fread(ds->data, sizeof(i32), 
          ds->header.num_sequences * ds->header.seq_length, f);
    
    ds->owns_memory = true;
    
    fclose(f);
    return ds;
}

const i32* dataset_get_sequence(Dataset* ds, i32 index) {
    if (index < 0 || index >= ds->header.num_sequences) {
        return NULL;
    }
    return ds->data + (index * ds->header.seq_length);
}

void dataset_destroy(Dataset* ds) {
    if (!ds) return;
    
    if (ds->owns_memory && ds->data) {
        aligned_free(ds->data);
    }
    free(ds);
}

// ===== UTILITIES =====

// Fisher-Yates shuffle
void dataset_shuffle(Dataset* ds) {
    i32 seq_len = ds->header.seq_length;
    
    for (i32 i = ds->header.num_sequences - 1; i > 0; i--) {
        i32 j = rand() % (i + 1);
        
        // Swap sequences i and j
        for (i32 k = 0; k < seq_len; k++) {
            i32 temp = ds->data[i * seq_len + k];
            ds->data[i * seq_len + k] = ds->data[j * seq_len + k];
            ds->data[j * seq_len + k] = temp;
        }
    }
}

Dataset** dataset_split(Dataset* input, f32 train_ratio, f32 val_ratio, f32 test_ratio) {
    // Validate ratios
    f32 sum = train_ratio + val_ratio + test_ratio;
    if (sum < 0.99f || sum > 1.01f) {
        return NULL;
    }
    
    i32 total = input->header.num_sequences;
    i32 train_size = (i32)(total * train_ratio);
    i32 val_size = (i32)(total * val_ratio);
    i32 test_size = total - train_size - val_size;
    
    Dataset** splits = (Dataset**)malloc(3 * sizeof(Dataset*));
    
    // Create datasets
    splits[0] = dataset_create(train_size, input->header.seq_length, input->header.vocab_size);
    splits[1] = dataset_create(val_size, input->header.seq_length, input->header.vocab_size);
    splits[2] = dataset_create(test_size, input->header.seq_length, input->header.vocab_size);
    
    // Copy data
    i32 seq_len = input->header.seq_length;
    i32 offset = 0;
    
    memcpy(splits[0]->data, input->data, train_size * seq_len * sizeof(i32));
    offset += train_size;
    
    memcpy(splits[1]->data, input->data + offset * seq_len, val_size * seq_len * sizeof(i32));
    offset += val_size;
    
    memcpy(splits[2]->data, input->data + offset * seq_len, test_size * seq_len * sizeof(i32));
    
    return splits;
}
