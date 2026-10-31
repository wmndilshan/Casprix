#include "token_stream.h"
#include <stdio.h>
#include <stdlib.h>

#define CPX_TOKEN_DATASET_MAGIC 0x54494E59u

typedef struct TokenDatasetHeader {
    i32 magic, version, num_sequences, sequence_length, vocabulary_size;
    i32 reserved[3];
} TokenDatasetHeader;

typedef struct TokenStream {
    FILE* file;
    TokenDatasetHeader header;
    i32 batch_size;
    i32 next_sequence;
} TokenStream;

typedef struct TokenBatch {
    i32* inputs;
    i32* targets;
    i32 rows;
} TokenBatch;

void* cpx_ai_token_stream_open(const char* path, i32 batch_size) {
    if (!path || batch_size <= 0) return NULL;
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    TokenStream* stream = (TokenStream*)calloc(1, sizeof(*stream));
    if (!stream || fread(&stream->header, sizeof(stream->header), 1, file) != 1 ||
        stream->header.magic != (i32)CPX_TOKEN_DATASET_MAGIC ||
        stream->header.version != 1 || stream->header.num_sequences < batch_size ||
        stream->header.sequence_length <= 1 || stream->header.vocabulary_size <= 0) {
        free(stream); fclose(file); return NULL;
    }
    stream->file = file; stream->batch_size = batch_size;
    return stream;
}

void* cpx_ai_token_stream_next(void* opaque) {
    TokenStream* stream = (TokenStream*)opaque;
    if (!stream) return NULL;
    if (stream->next_sequence + stream->batch_size > stream->header.num_sequences) {
        stream->next_sequence = 0;
        if (fseek(stream->file, (long)sizeof(TokenDatasetHeader), SEEK_SET) != 0) return NULL;
    }
    i32 rows = stream->batch_size * stream->header.sequence_length;
    TokenBatch* batch = (TokenBatch*)calloc(1, sizeof(*batch));
    if (!batch) return NULL;
    batch->inputs = (i32*)malloc((size_t)rows * sizeof(i32));
    batch->targets = (i32*)malloc((size_t)rows * sizeof(i32));
    batch->rows = rows;
    if (!batch->inputs || !batch->targets ||
        fread(batch->inputs, sizeof(i32), (size_t)rows, stream->file) != (size_t)rows) {
        cpx_ai_token_batch_destroy(batch); return NULL;
    }
    i32 sequence = stream->header.sequence_length;
    for (i32 item = 0; item < stream->batch_size; ++item) {
        i32 offset = item * sequence;
        for (i32 position = 0; position < sequence - 1; ++position)
            batch->targets[offset + position] = batch->inputs[offset + position + 1];
        batch->targets[offset + sequence - 1] = 0;
    }
    stream->next_sequence += stream->batch_size;
    return batch;
}

i32 cpx_ai_token_stream_sequence_length(void* opaque) {
    TokenStream* stream = (TokenStream*)opaque;
    return stream ? stream->header.sequence_length : 0;
}
i32 cpx_ai_token_stream_vocabulary_size(void* opaque) {
    TokenStream* stream = (TokenStream*)opaque;
    return stream ? stream->header.vocabulary_size : 0;
}
void cpx_ai_token_stream_close(void* opaque) {
    TokenStream* stream = (TokenStream*)opaque;
    if (!stream) return;
    if (stream->file) fclose(stream->file);
    free(stream);
}

static AiValue* batch_value(TokenBatch* batch, AiGraph* graph, const i32* source) {
    if (!batch || !graph || !source) return NULL;
    f32* values = (f32*)malloc((size_t)batch->rows * sizeof(f32));
    if (!values) return NULL;
    for (i32 i = 0; i < batch->rows; ++i) values[i] = (f32)source[i];
    AiValue* result = ai_value_copy_ephemeral(graph, 2, (i32[]){batch->rows, 1}, values);
    free(values); return result;
}

void* cpx_ai_token_batch_inputs(void* opaque, void* graph) {
    TokenBatch* batch = (TokenBatch*)opaque;
    return batch_value(batch, (AiGraph*)graph, batch ? batch->inputs : NULL);
}
void* cpx_ai_token_batch_targets(void* opaque, void* graph) {
    TokenBatch* batch = (TokenBatch*)opaque;
    return batch_value(batch, (AiGraph*)graph, batch ? batch->targets : NULL);
}
void cpx_ai_token_batch_destroy(void* opaque) {
    TokenBatch* batch = (TokenBatch*)opaque;
    if (!batch) return;
    free(batch->inputs); free(batch->targets); free(batch);
}
