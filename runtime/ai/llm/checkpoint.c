/*
 * LLM Runtime - Model Checkpointing Implementation
 */

#include "checkpoint.h"
#include "tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

#define CHECKPOINT_MAGIC 0x4D4F4445  // "MODE"
#define CHECKPOINT_VERSION 1

// Helper: Save tensor to file
static bool save_tensor(FILE* f, const Tensor* tensor) {
    if (!f || !tensor) return false;
    
    // Write tensor metadata
    fwrite(&tensor->ndim, sizeof(i32), 1, f);
    fwrite(tensor->shape, sizeof(i32), tensor->ndim, f);
    fwrite(&tensor->size, sizeof(size_t), 1, f);
    
    // Write tensor data
    fwrite(tensor->data, sizeof(f32), tensor->size, f);
    
    return true;
}

// Helper: Load tensor from file
static Tensor* load_tensor(FILE* f) {
    if (!f) return NULL;
    
    // Read metadata
    i32 ndim;
    i32 shape[MAX_TENSOR_DIM];
    size_t size;
    
    fread(&ndim, sizeof(i32), 1, f);
    fread(shape, sizeof(i32), ndim, f);
    fread(&size, sizeof(size_t), 1, f);
    
    // Create tensor
    Tensor* tensor = tensor_create(ndim, shape);
    if (!tensor) return NULL;
    
    // Read data
    fread(tensor->data, sizeof(f32), size, f);
    
    return tensor;
}

bool checkpoint_save(const TransformerModel* model, 
                     const CheckpointHeader* header,
                     const char* path) {
    if (!model || !header || !path) return false;
    
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open checkpoint file for writing: %s\n", path);
        return false;
    }
    
    // Write header
    fwrite(header, sizeof(CheckpointHeader), 1, f);
    
    // Write embeddings
    save_tensor(f, model->embeddings->token_embedding);
    save_tensor(f, model->embeddings->pos_embedding);
    
    // Write transformer blocks
    for (i32 i = 0; i < model->num_layers; i++) {
        TransformerBlock* block = model->blocks[i];
        
        // Attention (no biases)
        save_tensor(f, block->attn->Wq);
        save_tensor(f, block->attn->Wk);
        save_tensor(f, block->attn->Wv);
        save_tensor(f, block->attn->Wo);
        
        // LayerNorm 1
        save_tensor(f, block->ln1_gamma);
        save_tensor(f, block->ln1_beta);
        
        // FFN
        save_tensor(f, block->ffn->W1);
        save_tensor(f, block->ffn->b1);
        save_tensor(f, block->ffn->W2);
        save_tensor(f, block->ffn->b2);
        
        // LayerNorm 2
        save_tensor(f, block->ln2_gamma);
        save_tensor(f, block->ln2_beta);
    }
    
    // Write final layer norm
    save_tensor(f, model->ln_final_gamma);
    save_tensor(f, model->ln_final_beta);
    
    // Write LM head
    save_tensor(f, model->lm_head);
    
    fclose(f);
    printf("Checkpoint saved: %s\n", path);
    return true;
}

TransformerModel* checkpoint_load(const char* path, 
                                  CheckpointHeader* header_out) {
    if (!path) return NULL;
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open checkpoint file: %s\n", path);
        return NULL;
    }
    
    // Read header
    CheckpointHeader header;
    fread(&header, sizeof(CheckpointHeader), 1, f);
    
    // Validate magic
    if (header.magic != CHECKPOINT_MAGIC) {
        fprintf(stderr, "Invalid checkpoint file (bad magic)\n");
        fclose(f);
        return NULL;
    }
    
    if (header_out) {
        *header_out = header;
    }
    
    // Create model structure
    TransformerModel* model = (TransformerModel*)malloc(sizeof(TransformerModel));
    model->vocab_size = header.vocab_size;
    model->hidden_dim = header.hidden_dim;
    model->num_layers = header.num_layers;
    
    // Load embeddings
    model->embeddings = (Embeddings*)malloc(sizeof(Embeddings));
    model->embeddings->vocab_size = header.vocab_size;
    model->embeddings->hidden_dim = header.hidden_dim;
    model->embeddings->max_seq_len = header.max_seq_len;
    
    model->embeddings->token_embedding = load_tensor(f);
    model->embeddings->pos_embedding = load_tensor(f);
    
    // Load transformer blocks
    model->blocks = (TransformerBlock**)malloc(header.num_layers * sizeof(TransformerBlock*));
    
    for (i32 i = 0; i < header.num_layers; i++) {
        TransformerBlock* block = (TransformerBlock*)malloc(sizeof(TransformerBlock));
        
        // Attention
        block->attn = (Attention*)malloc(sizeof(Attention));
        block->attn->num_heads = header.num_heads;
        block->attn->head_dim = header.hidden_dim / header.num_heads;
        
        block->attn->Wq = load_tensor(f);
        block->attn->Wk = load_tensor(f);
        block->attn->Wv = load_tensor(f);
        block->attn->Wo = load_tensor(f);
        
        // LayerNorm 1
        block->ln1_gamma = load_tensor(f);
        block->ln1_beta = load_tensor(f);
        
        // FFN
        block->ffn = (FFN*)malloc(sizeof(FFN));
        
        block->ffn->W1 = load_tensor(f);
        block->ffn->b1 = load_tensor(f);
        block->ffn->W2 = load_tensor(f);
        block->ffn->b2 = load_tensor(f);
        
        // LayerNorm 2
        block->ln2_gamma = load_tensor(f);
        block->ln2_beta = load_tensor(f);
        
        model->blocks[i] = block;
    }
    
    // Load final layer norm
    model->ln_final_gamma = load_tensor(f);
    model->ln_final_beta = load_tensor(f);
    
    // Load LM head
    model->lm_head = load_tensor(f);
    
    fclose(f);
    printf("Checkpoint loaded: %s (step %d, loss %.4f)\n", 
           path, header.training_step, header.last_loss);
    
    return model;
}

bool checkpoint_save_weights_only(const TransformerModel* model,
                                  const char* path) {
    CheckpointHeader header = {0};
    header.magic = CHECKPOINT_MAGIC;
    header.version = CHECKPOINT_VERSION;
    header.vocab_size = model->vocab_size;
    header.hidden_dim = model->hidden_dim;
    header.num_layers = model->num_layers;
    
    // Extract config from first block (if exists)
    if (model->num_layers > 0 && model->blocks[0]->attn) {
        header.num_heads = model->blocks[0]->attn->num_heads;
    }
    // Compute ffn_dim from tensor shape
    if (model->num_layers > 0 && model->blocks[0]->ffn && model->blocks[0]->ffn->W1) {
        header.ffn_dim = model->blocks[0]->ffn->W1->shape[1];
    }
    if (model->embeddings) {
        header.max_seq_len = model->embeddings->max_seq_len;
    }
    
    return checkpoint_save(model, &header, path);
}

bool checkpoint_load_weights(TransformerModel* model, const char* path) {
    // Load checkpoint into temporary model
    CheckpointHeader header;
    TransformerModel* loaded = checkpoint_load(path, &header);
    
    if (!loaded) return false;
    
    // Copy weights (simplified - full implementation would validate dimensions)
    // For now, assume models have same architecture
    
    // Copy embeddings
    tensor_copy(loaded->embeddings->token_embedding, 
                model->embeddings->token_embedding);
    tensor_copy(loaded->embeddings->pos_embedding,
                model->embeddings->pos_embedding);
    
    // Copy blocks
    for (i32 i = 0; i < model->num_layers; i++) {
        TransformerBlock* dst = model->blocks[i];
        TransformerBlock* src = loaded->blocks[i];
        
        // Copy attention weights
        tensor_copy(src->attn->Wq, dst->attn->Wq);
        tensor_copy(src->attn->Wk, dst->attn->Wk);
        tensor_copy(src->attn->Wv, dst->attn->Wv);
        tensor_copy(src->attn->Wo, dst->attn->Wo);
        
        // Copy FFN weights
        tensor_copy(src->ffn->W1, dst->ffn->W1);
        tensor_copy(src->ffn->W2, dst->ffn->W2);
        
        // Copy layer norms
        tensor_copy(src->ln1_gamma, dst->ln1_gamma);
        tensor_copy(src->ln1_beta, dst->ln1_beta);
        tensor_copy(src->ln2_gamma, dst->ln2_gamma);
        tensor_copy(src->ln2_beta, dst->ln2_beta);
    }
    
    // Copy final layers
    tensor_copy(loaded->ln_final_gamma, model->ln_final_gamma);
    tensor_copy(loaded->ln_final_beta, model->ln_final_beta);
    tensor_copy(loaded->lm_head, model->lm_head);
    
    // Free temporary model
    // (Would need proper cleanup function)
    
    return true;
}
