/**
 * Multi-Head Attention Implementation
 */

#include "attention.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <math.h>

MultiHeadAttention* attention_create(int embed_dim, int num_heads, float dropout_rate) {
    MultiHeadAttention* attn = (MultiHeadAttention*)calloc(1, sizeof(MultiHeadAttention));
    
    attn->embed_dim = embed_dim;
    attn->num_heads = num_heads;
    attn->head_dim = embed_dim / num_heads;
    attn->dropout_rate = dropout_rate;
    
    /* Create projection layers */
    attn->query_proj = dense_create(embed_dim, embed_dim, true);
    attn->key_proj = dense_create(embed_dim, embed_dim, true);
    attn->value_proj = dense_create(embed_dim, embed_dim, true);
    attn->out_proj = dense_create(embed_dim, embed_dim, true);
    
    /* Initialize with Xavier */
    dense_init_weights(attn->query_proj, "xavier");
    dense_init_weights(attn->key_proj, "xavier");
    dense_init_weights(attn->value_proj, "xavier");
    dense_init_weights(attn->out_proj, "xavier");
    
    return attn;
}

void attention_forward(MultiHeadAttention* attn,
                      const Tensor* query, const Tensor* key, const Tensor* value,
                      const Tensor* mask, Tensor* output) {
    /* query, key, value: [batch, seq_len, embed_dim] */
    int batch = query->shape[0];
    int seq_len = query->shape[1];
    int embed_dim = attn->embed_dim;
    int num_heads = attn->num_heads;
    int head_dim = attn->head_dim;
    
    /* Project Q, K, V */
    int proj_shape[] = {batch, seq_len, embed_dim};
    Tensor* Q = tensor_create(3, proj_shape);
    Tensor* K = tensor_create(3, proj_shape);
    Tensor* V = tensor_create(3, proj_shape);
    
    /* Reshape to 2D for dense forward */
    int flat_shape[] = {batch * seq_len, embed_dim};
    Tensor query_2d = *query;
    query_2d.ndim = 2;
    query_2d.shape[0] = batch * seq_len;
    query_2d.shape[1] = embed_dim;
    
    Tensor Q_2d = *Q;
    Q_2d.ndim = 2;
    Q_2d.shape[0] = batch * seq_len;
    Q_2d.shape[1] = embed_dim;
    
    dense_forward(attn->query_proj, &query_2d, &Q_2d);
    
    /* Same for K and V */
    Tensor key_2d = *key;
    key_2d.ndim = 2;
    key_2d.shape[0] = batch * seq_len;
    key_2d.shape[1] = embed_dim;
    
    Tensor K_2d = *K;
    K_2d.ndim = 2;
    K_2d.shape[0] = batch * seq_len;
    K_2d.shape[1] = embed_dim;
    
    dense_forward(attn->key_proj, &key_2d, &K_2d);
    
    Tensor value_2d = *value;
    value_2d.ndim = 2;
    value_2d.shape[0] = batch * seq_len;
    value_2d.shape[1] = embed_dim;
    
    Tensor V_2d = *V;
    V_2d.ndim = 2;
    V_2d.shape[0] = batch * seq_len;
    V_2d.shape[1] = embed_dim;
    
    dense_forward(attn->value_proj, &value_2d, &V_2d);
    
    /* Reshape to [batch, num_heads, seq_len, head_dim] for multi-head */
    /* Simplified: Process as [batch * num_heads, seq_len, head_dim] */
    
    /* Compute attention scores: Q @ K^T / sqrt(head_dim) */
    float scale = 1.0f / sqrtf((float)head_dim);
    
    int scores_shape[] = {batch, seq_len, seq_len};
    Tensor* scores = tensor_create(3, scores_shape);
    
    /* Simplified single-head attention for demonstration */
    for (int b = 0; b < batch; b++) {
        float* Q_batch = Q->data + b * seq_len * embed_dim;
        float* K_batch = K->data + b * seq_len * embed_dim;
        float* scores_batch = scores->data + b * seq_len * seq_len;
        
        /* Q @ K^T */
        for (int i = 0; i < seq_len; i++) {
            for (int j = 0; j < seq_len; j++) {
                float sum = 0.0f;
                for (int d = 0; d < embed_dim; d++) {
                    sum += Q_batch[i * embed_dim + d] * K_batch[j * embed_dim + d];
                }
                scores_batch[i * seq_len + j] = sum * scale;
            }
        }
    }
    
    /* Apply mask if present */
    if (mask) {
        for (int i = 0; i < scores->size; i++) {
            if (mask->data[i] == 0.0f) {
                scores->data[i] = -1e9f;  /* Large negative number */
            }
        }
    }
    
    /* Softmax over last dimension */
    for (int b = 0; b < batch; b++) {
        for (int i = 0; i < seq_len; i++) {
            float* row = scores->data + b * seq_len * seq_len + i * seq_len;
            softmax_f32(row, row, 1, seq_len);
        }
    }
    
    /* Attention @ V */
    Tensor* context = tensor_create(3, proj_shape);
    
    for (int b = 0; b < batch; b++) {
        float* attn_weights = scores->data + b * seq_len * seq_len;
        float* V_batch = V->data + b * seq_len * embed_dim;
        float* out_batch = context->data + b * seq_len * embed_dim;
        
        /* Matrix multiply: [seq_len, seq_len] @ [seq_len, embed_dim] */
        gemm_f32(attn_weights, V_batch, out_batch, seq_len, seq_len, embed_dim);
    }
    
    /* Output projection */
    Tensor context_2d = *context;
    context_2d.ndim = 2;
    context_2d.shape[0] = batch * seq_len;
    context_2d.shape[1] = embed_dim;
    
    Tensor output_2d = *output;
    output_2d.ndim = 2;
    output_2d.shape[0] = batch * seq_len;
    output_2d.shape[1] = embed_dim;
    
    dense_forward(attn->out_proj, &context_2d, &output_2d);
    
    /* Cleanup */
    tensor_destroy(Q);
    tensor_destroy(K);
    tensor_destroy(V);
    tensor_destroy(scores);
    tensor_destroy(context);
}

void attention_free(MultiHeadAttention* attn) {
    if (!attn) return;
    
    dense_free(attn->query_proj);
    dense_free(attn->key_proj);
    dense_free(attn->value_proj);
    dense_free(attn->out_proj);
    
    if (attn->attention_weights) {
        tensor_destroy(attn->attention_weights);
    }
    
    free(attn);
}
