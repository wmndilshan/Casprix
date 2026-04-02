/*
 * LLM Runtime - Model Checkpointing
 * 
 * Save and load transformer model states for training persistence
 */

#ifndef LLM_CHECKPOINT_H
#define LLM_CHECKPOINT_H

#include "transformer.h"
#include <stdbool.h>

// Version history:
//   v1 = original format (magic + weights, no checksum)
//   v2 = added sha256[32] field + version enforcement on load
#define CHECKPOINT_CURRENT_VERSION  2
#define CHECKPOINT_SHA256_SIZE      32

// Checkpoint header
typedef struct CheckpointHeader {
    u32 magic;              // 0x4D4F4445 ("MODE")
    u32 version;            // Format version — use CHECKPOINT_CURRENT_VERSION

    // Model architecture
    i32 vocab_size;
    i32 hidden_dim;
    i32 num_layers;
    i32 num_heads;
    i32 ffn_dim;
    i32 max_seq_len;

    // Training state
    i32 training_step;
    f32 learning_rate;
    f32 last_loss;

    // SHA-256 of all payload bytes after this header.
    // Zero for v1 files (no checksum).
    u8  sha256[CHECKPOINT_SHA256_SIZE];

    // Reserved for future use
    i32 reserved[8];
} CheckpointHeader;

// Compute SHA-256 digest of data[0..len) into digest[32].
// Pure C99 — no external library dependency.
void checkpoint_sha256(const void* data, size_t len, u8 digest[CHECKPOINT_SHA256_SIZE]);

// Save model checkpoint
// Saves all parameters, optimizer state, and training metadata
bool checkpoint_save(const TransformerModel* model, 
                     const CheckpointHeader* header,
                     const char* path);

// Load model checkpoint
// Returns loaded model or NULL on failure
TransformerModel* checkpoint_load(const char* path, 
                                  CheckpointHeader* header_out);

// Save just the model weights (for inference)
bool checkpoint_save_weights_only(const TransformerModel* model,
                                  const char* path);

// Load weights into existing model
bool checkpoint_load_weights(TransformerModel* model,
                            const char* path);

#endif // LLM_CHECKPOINT_H
