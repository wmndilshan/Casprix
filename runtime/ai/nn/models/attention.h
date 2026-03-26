/**
 * Multi-Head Attention
 * 
 * Core building block for Transformers
 */

#ifndef NN_ATTENTION_H
#define NN_ATTENTION_H

#include "../../llm/tensor.h"
#include "../layers/dense.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int embed_dim;      /* Total embedding dimension */
    int num_heads;      /* Number of attention heads */
    int head_dim;       /* Dimension per head (embed_dim / num_heads) */
    
    /* Projections */
    DenseLayer* query_proj;
    DenseLayer* key_proj;
    DenseLayer* value_proj;
    DenseLayer* out_proj;
    
    /* Dropout (optional) */
    float dropout_rate;
    
    /* Cached for backward */
    Tensor* attention_weights;
} MultiHeadAttention;

/**
 * Create multi-head attention layer
 * @param embed_dim Embedding dimension
 * @param num_heads Number of attention heads (embed_dim must be divisible)
 * @param dropout_rate Dropout rate for attention weights (0.0 = no dropout)
 */
MultiHeadAttention* attention_create(int embed_dim, int num_heads, float dropout_rate);

/**
 * Forward pass (self-attention)
 * @param query Query tensor [batch, seq_len, embed_dim]
 * @param key Key tensor [batch, seq_len, embed_dim]
 * @param value Value tensor [batch, seq_len, embed_dim]
 * @param mask Optional attention mask [batch, seq_len, seq_len] (NULL for no mask)
 * @param output Output tensor [batch, seq_len, embed_dim]
 */
void attention_forward(MultiHeadAttention* attn,
                      const Tensor* query, const Tensor* key, const Tensor* value,
                      const Tensor* mask, Tensor* output);

/**
 * Free attention layer
 */
void attention_free(MultiHeadAttention* attn);

#ifdef __cplusplus
}
#endif

#endif /* NN_ATTENTION_H */
