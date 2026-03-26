/*
 * LLM Runtime - Transformer Implementation
 */

#include "transformer.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// ===== RANDOM INITIALIZATION HELPER =====

static void random_init(Tensor* tensor, f32 scale) {
    for (size_t i = 0; i < tensor->size; i++) {
        // Simple random init (replace with proper Xavier/He init)
        tensor->data[i] = ((f32)rand() / RAND_MAX - 0.5f) * 2.0f * scale;
    }
}

// ===== ATTENTION =====

Attention* attention_create(i32 hidden_dim, i32 num_heads) {
    Attention* attn = (Attention*)malloc(sizeof(Attention));
    
    attn->num_heads = num_heads;
    attn->head_dim = hidden_dim / num_heads;
    
    // Create weight matrices
    attn->Wq = tensor_create(2, (i32[]){hidden_dim, hidden_dim});
    attn->Wk = tensor_create(2, (i32[]){hidden_dim, hidden_dim});
    attn->Wv = tensor_create(2, (i32[]){hidden_dim, hidden_dim});
    attn->Wo = tensor_create(2, (i32[]){hidden_dim, hidden_dim});
    
    // Initialize weights
    f32 scale = 1.0f / sqrtf((f32)hidden_dim);
    random_init(attn->Wq, scale);
    random_init(attn->Wk, scale);
    random_init(attn->Wv, scale);
    random_init(attn->Wo, scale);
    
    return attn;
}

void attention_forward(Attention* attn, const Tensor* x, Tensor* out, Arena* arena) {
    i32 batch = x->shape[0];
    i32 seq_len = x->shape[1];
    i32 hidden_dim = x->shape[2];
    
    // Q, K, V projections: [batch*seq_len, hidden_dim] × [hidden_dim, hidden_dim]
    Tensor* Q = arena_alloc_tensor(arena, 3, (i32[]){batch, seq_len, hidden_dim});
    Tensor* K = arena_alloc_tensor(arena, 3, (i32[]){batch, seq_len, hidden_dim});
    Tensor* V = arena_alloc_tensor(arena, 3, (i32[]){batch, seq_len, hidden_dim});
    
    gemm_f32(x->data, attn->Wq->data, Q->data, batch*seq_len, hidden_dim, hidden_dim);
    gemm_f32(x->data, attn->Wk->data, K->data, batch*seq_len, hidden_dim, hidden_dim);
    gemm_f32(x->data, attn->Wv->data, V->data, batch*seq_len, hidden_dim, hidden_dim);
    
    // For simplicity: single-head attention (full multi-head requires reshape)
    // Attention scores: Q * K^T / sqrt(head_dim)
    // [batch, seq_len, hidden_dim] × [batch, hidden_dim, seq_len] -> [batch, seq_len, seq_len]
    
    // Simplified: process per batch
    for (i32 b = 0; b < batch; b++) {
        // Q_b: [seq_len, hidden_dim]
        f32* Q_b = Q->data + b * seq_len * hidden_dim;
        f32* K_b = K->data + b * seq_len * hidden_dim;
        f32* V_b = V->data + b * seq_len * hidden_dim;
        
        // Transpose K: [seq_len, hidden_dim] -> [hidden_dim, seq_len]
        Tensor* K_T = arena_alloc_tensor(arena, 2, (i32[]){hidden_dim, seq_len});
        transpose_f32(K_b, K_T->data, seq_len, hidden_dim);
        
        // Scores: Q_b * K_T = [seq_len, hidden_dim] × [hidden_dim, seq_len] -> [seq_len, seq_len]
        Tensor* scores = arena_alloc_tensor(arena, 2, (i32[]){seq_len, seq_len});
        gemm_f32(Q_b, K_T->data, scores->data, seq_len, hidden_dim, seq_len);
        
        // Scale by 1/sqrt(head_dim)
        f32 scale = 1.0f / sqrtf((f32)attn->head_dim);
        vec_scale_f32(scores->data, scale, scores->data, seq_len * seq_len);
        
        // Softmax per row
        softmax_f32(scores->data, scores->data, seq_len, seq_len);
        
        // Attention output: scores * V = [seq_len, seq_len] × [seq_len, hidden_dim] -> [seq_len, hidden_dim]
        Tensor* attn_out_b = arena_alloc_tensor(arena, 2, (i32[]){seq_len, hidden_dim});
        gemm_f32(scores->data, V_b, attn_out_b->data, seq_len, seq_len, hidden_dim);
        
        // Output projection
        f32* out_b = out->data + b * seq_len * hidden_dim;
        gemm_f32(attn_out_b->data, attn->Wo->data, out_b, seq_len, hidden_dim, hidden_dim);
    }
}

void attention_destroy(Attention* attn) {
    if (attn) {
        tensor_destroy(attn->Wq);
        tensor_destroy(attn->Wk);
        tensor_destroy(attn->Wv);
        tensor_destroy(attn->Wo);
        free(attn);
    }
}

// ===== FEED-FORWARD NETWORK =====

FFN* ffn_create(i32 hidden_dim, i32 ffn_dim) {
    FFN* ffn = (FFN*)malloc(sizeof(FFN));
    
    ffn->W1 = tensor_create(2, (i32[]){hidden_dim, ffn_dim});
    ffn->b1 = tensor_create(1, (i32[]){ffn_dim});
    ffn->W2 = tensor_create(2, (i32[]){ffn_dim, hidden_dim});
    ffn->b2 = tensor_create(1, (i32[]){hidden_dim});
    
    f32 scale1 = 1.0f / sqrtf((f32)hidden_dim);
    f32 scale2 = 1.0f / sqrtf((f32)ffn_dim);
    random_init(ffn->W1, scale1);
    random_init(ffn->W2, scale2);
    tensor_zeros(ffn->b1);
    tensor_zeros(ffn->b2);
    
    return ffn;
}

void ffn_forward(FFN* ffn, const Tensor* x, Tensor* out, Arena* arena) {
    i32 batch_seq = x->shape[0] * x->shape[1];
    i32 hidden_dim = x->shape[2];
    i32 ffn_dim = ffn->W1->shape[1];
    
    // First layer: x * W1
    Tensor* hidden = arena_alloc_tensor(arena, 2, (i32[]){batch_seq, ffn_dim});
    gemm_f32(x->data, ffn->W1->data, hidden->data, batch_seq, hidden_dim, ffn_dim);
    
    // Add bias
    for (i32 i = 0; i < batch_seq; i++) {
        vec_add_f32(hidden->data + i*ffn_dim, ffn->b1->data, 
                    hidden->data + i*ffn_dim, ffn_dim);
    }
    
    // GELU activation
    gelu_f32(hidden->data, hidden->data, batch_seq * ffn_dim);
    
    // Second layer: hidden * W2
    gemm_f32(hidden->data, ffn->W2->data, out->data, batch_seq, ffn_dim, hidden_dim);
    
    // Add bias
    for (i32 i = 0; i < batch_seq; i++) {
        vec_add_f32(out->data + i*hidden_dim, ffn->b2->data,
                    out->data + i*hidden_dim, hidden_dim);
    }
}

void ffn_destroy(FFN* ffn) {
    if (ffn) {
        tensor_destroy(ffn->W1);
        tensor_destroy(ffn->b1);
        tensor_destroy(ffn->W2);
        tensor_destroy(ffn->b2);
        free(ffn);
    }
}

// ===== TRANSFORMER BLOCK =====

TransformerBlock* transformer_block_create(i32 hidden_dim, i32 num_heads, i32 ffn_dim) {
    TransformerBlock* block = (TransformerBlock*)malloc(sizeof(TransformerBlock));
    
    block->attn = attention_create(hidden_dim, num_heads);
    block->ffn = ffn_create(hidden_dim, ffn_dim);
    
    // LayerNorm parameters
    block->ln1_gamma = tensor_create(1, (i32[]){hidden_dim});
    block->ln1_beta = tensor_create(1, (i32[]){hidden_dim});
    block->ln2_gamma = tensor_create(1, (i32[]){hidden_dim});
    block->ln2_beta = tensor_create(1, (i32[]){hidden_dim});
    
    tensor_fill(block->ln1_gamma, 1.0f);
    tensor_zeros(block->ln1_beta);
    tensor_fill(block->ln2_gamma, 1.0f);
    tensor_zeros(block->ln2_beta);
    
    return block;
}

void transformer_block_forward(TransformerBlock* block, const Tensor* x, 
                                 Tensor* out, Arena* arena) {
    i32 batch_seq = x->shape[0] * x->shape[1];
    i32 hidden_dim = x->shape[2];
    
    // Pre-normalization
    Tensor* ln1_out = arena_alloc_tensor(arena, 3, x->shape);
    layernorm_f32(x->data, block->ln1_gamma->data, block->ln1_beta->data,
                  ln1_out->data, batch_seq, hidden_dim, 1e-5f);
    
    // Self-attention
    Tensor* attn_out = arena_alloc_tensor(arena, 3, x->shape);
    attention_forward(block->attn, ln1_out, attn_out, arena);
    
    // Residual connection
    Tensor* residual1 = arena_alloc_tensor(arena, 3, x->shape);
    vec_add_f32(x->data, attn_out->data, residual1->data, x->size);
    
    // Pre-norm for FFN
    Tensor* ln2_out = arena_alloc_tensor(arena, 3, x->shape);
    layernorm_f32(residual1->data, block->ln2_gamma->data, block->ln2_beta->data,
                  ln2_out->data, batch_seq, hidden_dim, 1e-5f);
    
    // FFN
    Tensor* ffn_out = arena_alloc_tensor(arena, 3, x->shape);
    ffn_forward(block->ffn, ln2_out, ffn_out, arena);
    
    // Final residual
    vec_add_f32(residual1->data, ffn_out->data, out->data, x->size);
}

void transformer_block_destroy(TransformerBlock* block) {
    if (block) {
        attention_destroy(block->attn);
        ffn_destroy(block->ffn);
        tensor_destroy(block->ln1_gamma);
        tensor_destroy(block->ln1_beta);
        tensor_destroy(block->ln2_gamma);
        tensor_destroy(block->ln2_beta);
        free(block);
    }
}

// ===== EMBEDDINGS =====

Embeddings* embeddings_create(i32 vocab_size, i32 hidden_dim, i32 max_seq_len) {
    Embeddings* emb = (Embeddings*)malloc(sizeof(Embeddings));
    
    emb->vocab_size = vocab_size;
    emb->hidden_dim = hidden_dim;
    emb->max_seq_len = max_seq_len;
    
    emb->token_embedding = tensor_create(2, (i32[]){vocab_size, hidden_dim});
    emb->pos_embedding = tensor_create(2, (i32[]){max_seq_len, hidden_dim});
    
    random_init(emb->token_embedding, 0.02f);
    random_init(emb->pos_embedding, 0.02f);
    
    return emb;
}

void embeddings_forward(Embeddings* emb, const i32* token_ids, 
                        i32 batch, i32 seq_len, Tensor* out) {
    // Token embeddings
    embedding_lookup(emb->token_embedding->data, token_ids, out->data,
                     batch, seq_len, emb->vocab_size, emb->hidden_dim);
    
    // Add positional embeddings
    for (i32 b = 0; b < batch; b++) {
        for (i32 s = 0; s < seq_len; s++) {
            f32* out_ptr = out->data + (b * seq_len + s) * emb->hidden_dim;
            f32* pos_ptr = emb->pos_embedding->data + s * emb->hidden_dim;
            vec_add_f32(out_ptr, pos_ptr, out_ptr, emb->hidden_dim);
        }
    }
}

void embeddings_destroy(Embeddings* emb) {
    if (emb) {
        tensor_destroy(emb->token_embedding);
        tensor_destroy(emb->pos_embedding);
        free(emb);
    }
}

// ===== FULL TRANSFORMER MODEL =====

TransformerModel* transformer_create(i32 vocab_size, i32 hidden_dim, 
                                      i32 num_layers, i32 num_heads, 
                                      i32 ffn_dim, i32 max_seq_len) {
    TransformerModel* model = (TransformerModel*)malloc(sizeof(TransformerModel));
    
    model->vocab_size = vocab_size;
    model->hidden_dim = hidden_dim;
    model->num_layers = num_layers;
    
    // Embeddings
    model->embeddings = embeddings_create(vocab_size, hidden_dim, max_seq_len);
    
    // Transformer blocks
    model->blocks = (TransformerBlock**)malloc(num_layers * sizeof(TransformerBlock*));
    for (i32 i = 0; i < num_layers; i++) {
        model->blocks[i] = transformer_block_create(hidden_dim, num_heads, ffn_dim);
    }
    
    // Final LayerNorm
    model->ln_final_gamma = tensor_create(1, (i32[]){hidden_dim});
    model->ln_final_beta = tensor_create(1, (i32[]){hidden_dim});
    tensor_fill(model->ln_final_gamma, 1.0f);
    tensor_zeros(model->ln_final_beta);
    
    // LM head (language model projection to vocab)
    model->lm_head = tensor_create(2, (i32[]){hidden_dim, vocab_size});
    random_init(model->lm_head, 0.02f);
    
    return model;
}

void transformer_forward(TransformerModel* model, const i32* token_ids,
                         i32 batch, i32 seq_len, Tensor* logits,
                         MemoryPools* mem) {
    i32 hidden_dim = model->hidden_dim;
    
    // Embedding
    Tensor* x = arena_alloc_tensor(mem->activations, 3, (i32[]){batch, seq_len, hidden_dim});
    embeddings_forward(model->embeddings, token_ids, batch, seq_len, x);
    
    // Transformer blocks
    Tensor* block_out = arena_alloc_tensor(mem->activations, 3, (i32[]){batch, seq_len, hidden_dim});
    for (i32 i = 0; i < model->num_layers; i++) {
        transformer_block_forward(model->blocks[i], x, block_out, mem->activations);
        // Swap for next layer
        Tensor* temp = x;
        x = block_out;
        block_out = temp;
    }
    
    // Final LayerNorm
    Tensor* ln_out = arena_alloc_tensor(mem->activations, 3, (i32[]){batch, seq_len, hidden_dim});
    layernorm_f32(x->data, model->ln_final_gamma->data, model->ln_final_beta->data,
                  ln_out->data, batch*seq_len, hidden_dim, 1e-5f);
    
    // Project to vocab: [batch*seq_len, hidden_dim] × [hidden_dim, vocab_size]
    gemm_f32(ln_out->data, model->lm_head->data, logits->data, 
             batch*seq_len, hidden_dim, model->vocab_size);
}

void transformer_destroy(TransformerModel* model) {
    if (model) {
        embeddings_destroy(model->embeddings);
        for (i32 i = 0; i < model->num_layers; i++) {
            transformer_block_destroy(model->blocks[i]);
        }
        free(model->blocks);
        tensor_destroy(model->ln_final_gamma);
        tensor_destroy(model->ln_final_beta);
        tensor_destroy(model->lm_head);
        free(model);
    }
}
