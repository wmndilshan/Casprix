/*
 * LLM Runtime - Transformer Components
 */

#ifndef LLM_TRANSFORMER_H
#define LLM_TRANSFORMER_H

#include "tensor.h"
#include "ops.h"

// ===== ATTENTION =====

typedef struct Attention {
    Tensor* Wq;           // Query weights [hidden_dim, hidden_dim]
    Tensor* Wk;           // Key weights [hidden_dim, hidden_dim]
    Tensor* Wv;           // Value weights [hidden_dim, hidden_dim]
    Tensor* Wo;           // Output weights [hidden_dim, hidden_dim]
    i32 num_heads;
    i32 head_dim;         // hidden_dim / num_heads
} Attention;

// Create attention module
Attention* attention_create(i32 hidden_dim, i32 num_heads);

// Forward pass
// x: [batch, seq_len, hidden_dim]
// out: [batch, seq_len, hidden_dim]
void attention_forward(Attention* attn, const Tensor* x, Tensor* out, TensorArena* arena);

// Destroy attention module
void attention_destroy(Attention* attn);

// ===== FEED-FORWARD NETWORK =====

typedef struct FFN {
    Tensor* W1;           // [hidden_dim, ffn_dim]
    Tensor* b1;           // [ffn_dim]
    Tensor* W2;           // [ffn_dim, hidden_dim]
    Tensor* b2;           // [hidden_dim]
} FFN;

// Create FFN
FFN* ffn_create(i32 hidden_dim, i32 ffn_dim);

// Forward pass
// x: [batch, seq_len, hidden_dim]
// out: [batch, seq_len, hidden_dim]
void ffn_forward(FFN* ffn, const Tensor* x, Tensor* out, TensorArena* arena);

// Destroy FFN
void ffn_destroy(FFN* ffn);

// ===== TRANSFORMER BLOCK =====

typedef struct TransformerBlock {
    Attention* attn;
    FFN* ffn;
    Tensor* ln1_gamma;    // LayerNorm 1 scale
    Tensor* ln1_beta;     // LayerNorm 1 bias
    Tensor* ln2_gamma;    // LayerNorm 2 scale
    Tensor* ln2_beta;     // LayerNorm 2 bias
} TransformerBlock;

// Create transformer block
TransformerBlock* transformer_block_create(i32 hidden_dim, i32 num_heads, i32 ffn_dim);

// Forward pass with residual connections
// x: [batch, seq_len, hidden_dim]
// out: [batch, seq_len, hidden_dim]
void transformer_block_forward(TransformerBlock* block, const Tensor* x, 
                                 Tensor* out, TensorArena* arena);

// Destroy transformer block
void transformer_block_destroy(TransformerBlock* block);

// ===== EMBEDDINGS =====

typedef struct Embeddings {
    Tensor* token_embedding;   // [vocab_size, hidden_dim]
    Tensor* pos_embedding;     // [max_seq_len, hidden_dim]
    i32 vocab_size;
    i32 hidden_dim;
    i32 max_seq_len;
} Embeddings;

// Create embeddings
Embeddings* embeddings_create(i32 vocab_size, i32 hidden_dim, i32 max_seq_len);

// Forward: token + positional embeddings
// token_ids: [batch, seq_len]
// out: [batch, seq_len, hidden_dim]
void embeddings_forward(Embeddings* emb, const i32* token_ids, 
                        i32 batch, i32 seq_len, Tensor* out);

// Destroy embeddings
void embeddings_destroy(Embeddings* emb);

// ===== FULL TRANSFORMER MODEL =====

typedef struct TransformerModel {
    Embeddings* embeddings;
    TransformerBlock** blocks;
    i32 num_layers;
    Tensor* ln_final_gamma;    // Final LayerNorm
    Tensor* ln_final_beta;
    Tensor* lm_head;           // [hidden_dim, vocab_size] for logits
    i32 hidden_dim;
    i32 vocab_size;
} TransformerModel;

// Create transformer model
TransformerModel* transformer_create(i32 vocab_size, i32 hidden_dim, 
                                      i32 num_layers, i32 num_heads, 
                                      i32 ffn_dim, i32 max_seq_len);

// Forward pass (training or inference)
// token_ids: [batch, seq_len]
// logits: [batch, seq_len, vocab_size]
void transformer_forward(TransformerModel* model, const i32* token_ids,
                         i32 batch, i32 seq_len, Tensor* logits,
                         MemoryPools* mem);

// Destroy model
void transformer_destroy(TransformerModel* model);

#endif // LLM_TRANSFORMER_H
