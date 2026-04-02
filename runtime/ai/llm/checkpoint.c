/*
 * LLM Runtime - Model Checkpointing Implementation
 */

#include "checkpoint.h"
#include "tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

#define CHECKPOINT_MAGIC 0x4D4F4445  /* "MODE" */

/* ════════════════════════════════════════════════════════════════════
 * SHA-256 — pure C99 (public domain algorithm)
 * ════════════════════════════════════════════════════════════════════ */
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define SHA256_ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SHA256_CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define SHA256_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define SHA256_EP0(x) (SHA256_ROTR(x,2) ^SHA256_ROTR(x,13)^SHA256_ROTR(x,22))
#define SHA256_EP1(x) (SHA256_ROTR(x,6) ^SHA256_ROTR(x,11)^SHA256_ROTR(x,25))
#define SHA256_SG0(x) (SHA256_ROTR(x,7) ^SHA256_ROTR(x,18)^((x)>>3))
#define SHA256_SG1(x) (SHA256_ROTR(x,17)^SHA256_ROTR(x,19)^((x)>>10))

static void sha256_block(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    for (int i=0;i<16;i++)
        w[i]=((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)
            |((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i=16;i<64;i++)
        w[i]=SHA256_SG1(w[i-2])+w[i-7]+SHA256_SG0(w[i-15])+w[i-16];
    a=st[0];b=st[1];c=st[2];d=st[3];e=st[4];f=st[5];g=st[6];h=st[7];
    for (int i=0;i<64;i++){
        t1=h+SHA256_EP1(e)+SHA256_CH(e,f,g)+sha256_k[i]+w[i];
        t2=SHA256_EP0(a)+SHA256_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

void checkpoint_sha256(const void* data, size_t len, u8 digest[CHECKPOINT_SHA256_SIZE]) {
    uint32_t st[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t* p=(const uint8_t*)data;
    size_t rem=len;
    uint8_t blk[64];
    while(rem>=64){sha256_block(st,p);p+=64;rem-=64;}
    memcpy(blk,p,rem);
    blk[rem]=0x80;
    if(rem<56){memset(blk+rem+1,0,55-rem);}
    else{memset(blk+rem+1,0,63-rem);sha256_block(st,blk);memset(blk,0,56);}
    uint64_t bits=(uint64_t)len*8;
    for(int i=0;i<8;i++) blk[63-i]=(uint8_t)(bits>>(i*8));
    sha256_block(st,blk);
    for(int i=0;i<8;i++){
        digest[i*4  ]=(uint8_t)(st[i]>>24);
        digest[i*4+1]=(uint8_t)(st[i]>>16);
        digest[i*4+2]=(uint8_t)(st[i]>>8);
        digest[i*4+3]=(uint8_t)(st[i]);
    }
}

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

/* Write all model weights to a memory buffer so we can SHA-256 them,
 * then flush buffer + header to disk.  Returns true on success. */
bool checkpoint_save(const TransformerModel* model,
                     const CheckpointHeader* header,
                     const char* path) {
    if (!model || !header || !path) return false;

    /* ---- Phase 1: write payload to a temp file to get byte stream ---- */
    /* We write to a temp path first, read it back for checksum, then
     * rewrite the final file with the SHA-256 in the header.            */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* ft = fopen(tmp_path, "wb");
    if (!ft) return false;

    /* Reserve space for header (written after checksum is known). */
    CheckpointHeader hdr = *header;
    hdr.magic   = CHECKPOINT_MAGIC;
    hdr.version = CHECKPOINT_CURRENT_VERSION;
    memset(hdr.sha256, 0, CHECKPOINT_SHA256_SIZE);
    fwrite(&hdr, sizeof(CheckpointHeader), 1, ft);

    /* Payload start offset. */
    long payload_start = (long)sizeof(CheckpointHeader);

    save_tensor(ft, model->embeddings->token_embedding);
    save_tensor(ft, model->embeddings->pos_embedding);

    for (i32 i = 0; i < model->num_layers; i++) {
        TransformerBlock* block = model->blocks[i];
        save_tensor(ft, block->attn->Wq);
        save_tensor(ft, block->attn->Wk);
        save_tensor(ft, block->attn->Wv);
        save_tensor(ft, block->attn->Wo);
        save_tensor(ft, block->ln1_gamma);
        save_tensor(ft, block->ln1_beta);
        save_tensor(ft, block->ffn->W1);
        save_tensor(ft, block->ffn->b1);
        save_tensor(ft, block->ffn->W2);
        save_tensor(ft, block->ffn->b2);
        save_tensor(ft, block->ln2_gamma);
        save_tensor(ft, block->ln2_beta);
    }
    save_tensor(ft, model->ln_final_gamma);
    save_tensor(ft, model->ln_final_beta);
    save_tensor(ft, model->lm_head);
    fclose(ft);

    /* ---- Phase 2: compute SHA-256 over payload bytes ---- */
    ft = fopen(tmp_path, "rb");
    if (!ft) return false;
    fseek(ft, payload_start, SEEK_SET);
    fseek(ft, 0, SEEK_END);
    long total_size = ftell(ft);
    long payload_size = total_size - payload_start;
    fseek(ft, payload_start, SEEK_SET);

    u8* payload_buf = (u8*)malloc((size_t)payload_size);
    if (!payload_buf) { fclose(ft); return false; }
    if ((long)fread(payload_buf, 1, (size_t)payload_size, ft) != payload_size) {
        free(payload_buf); fclose(ft); return false;
    }
    fclose(ft);

    checkpoint_sha256(payload_buf, (size_t)payload_size, hdr.sha256);
    free(payload_buf);

    /* ---- Phase 3: write final file with correct header + SHA-256 ---- */
    FILE* ff = fopen(path, "wb");
    if (!ff) return false;
    fwrite(&hdr, sizeof(CheckpointHeader), 1, ff);

    /* Re-write payload from temp file. */
    ft = fopen(tmp_path, "rb");
    fseek(ft, payload_start, SEEK_SET);
    u8 copy_buf[8192];
    size_t n;
    while ((n = fread(copy_buf, 1, sizeof(copy_buf), ft)) > 0)
        fwrite(copy_buf, 1, n, ff);
    fclose(ft);
    fclose(ff);
    remove(tmp_path);

    return true;
}

TransformerModel* checkpoint_load(const char* path,
                                  CheckpointHeader* header_out) {
    if (!path) return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    /* Read and validate header. */
    CheckpointHeader header;
    if (fread(&header, sizeof(CheckpointHeader), 1, f) != 1) { fclose(f); return NULL; }

    if (header.magic != CHECKPOINT_MAGIC) { fclose(f); return NULL; }

    if (header.version < 1 || header.version > CHECKPOINT_CURRENT_VERSION) {
        fclose(f);
        return NULL;
    }

    /* Version 2+: verify SHA-256 of payload. */
    if (header.version >= 2) {
        long payload_start = (long)sizeof(CheckpointHeader);
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        long payload_size = file_size - payload_start;
        fseek(f, payload_start, SEEK_SET);

        u8* payload = (u8*)malloc((size_t)payload_size);
        if (!payload) { fclose(f); return NULL; }
        if ((long)fread(payload, 1, (size_t)payload_size, f) != payload_size) {
            free(payload); fclose(f); return NULL;
        }

        u8 computed[CHECKPOINT_SHA256_SIZE];
        checkpoint_sha256(payload, (size_t)payload_size, computed);
        free(payload);

        if (memcmp(computed, header.sha256, CHECKPOINT_SHA256_SIZE) != 0) {
            /* Checksum mismatch — file is corrupt or tampered. */
            fclose(f);
            return NULL;
        }

        /* Rewind to start of payload for normal tensor loading. */
        fseek(f, payload_start, SEEK_SET);
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
