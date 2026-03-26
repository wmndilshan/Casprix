/**
 * Model Checkpointing Implementation
 */

#include "checkpoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKPOINT_MAGIC 0x43504B54  /* "CPKT" */
#define CHECKPOINT_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_params;
    uint32_t metadata_len;
} CheckpointHeader;

bool checkpoint_save(const char* filepath, Tensor** params, int num_params,
                    const char* metadata) {
    FILE* fp = fopen(filepath, "wb");
    if (!fp) {
        return false;
    }
    
    /* Write header */
    CheckpointHeader header;
    header.magic = CHECKPOINT_MAGIC;
    header.version = CHECKPOINT_VERSION;
    header.num_params = num_params;
    header.metadata_len = metadata ? strlen(metadata) + 1 : 0;
    
    fwrite(&header, sizeof(CheckpointHeader), 1, fp);
    
    /* Write metadata if present */
    if (metadata) {
        fwrite(metadata, 1, header.metadata_len, fp);
    }
    
    /* Write each parameter */
    for (int i = 0; i < num_params; i++) {
        Tensor* tensor = params[i];
        
        /* Write tensor info */
        fwrite(&tensor->ndim, sizeof(int), 1, fp);
        fwrite(tensor->shape, sizeof(int), tensor->ndim, fp);
        fwrite(&tensor->size, sizeof(size_t), 1, fp);
        
        /* Write tensor data */
        fwrite(tensor->data, sizeof(float), tensor->size, fp);
    }
    
    fclose(fp);
    return true;
}

bool checkpoint_load(const char* filepath, Tensor** params, int num_params) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return false;
    }
    
    /* Read header */
    CheckpointHeader header;
    fread(&header, sizeof(CheckpointHeader), 1, fp);
    
    /* Validate */
    if (header.magic != CHECKPOINT_MAGIC) {
        fclose(fp);
        return false;
    }
    
    if (header.num_params != (uint32_t)num_params) {
        fclose(fp);
        return false;
    }
    
    /* Skip metadata */
    if (header.metadata_len > 0) {
        fseek(fp, header.metadata_len, SEEK_CUR);
    }
    
    /* Read each parameter */
    for (int i = 0; i < num_params; i++) {
        Tensor* tensor = params[i];
        
        /* Read tensor info */
        int ndim;
        int shape[MAX_TENSOR_DIM];
        size_t size;
        
        fread(&ndim, sizeof(int), 1, fp);
        fread(shape, sizeof(int), ndim, fp);
        fread(&size, sizeof(size_t), 1, fp);
        
        /* Validate tensor matches */
        if (ndim != tensor->ndim || size != tensor->size) {
            fclose(fp);
            return false;
        }
        
        /* Read tensor data */
        fread(tensor->data, sizeof(float), size, fp);
    }
    
    fclose(fp);
    return true;
}

bool checkpoint_get_metadata(const char* filepath, char** metadata_out) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return false;
    }
    
    /* Read header */
    CheckpointHeader header;
    fread(&header, sizeof(CheckpointHeader), 1, fp);
    
    if (header.magic != CHECKPOINT_MAGIC) {
        fclose(fp);
        return false;
    }
    
    if (header.metadata_len == 0) {
        *metadata_out = NULL;
        fclose(fp);
        return true;
    }
    
    /* Read metadata */
    *metadata_out = (char*)malloc(header.metadata_len);
    fread(*metadata_out, 1, header.metadata_len, fp);
    
    fclose(fp);
    return true;
}
