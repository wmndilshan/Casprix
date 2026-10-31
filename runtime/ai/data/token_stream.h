#ifndef CPX_AI_DATA_TOKEN_STREAM_H
#define CPX_AI_DATA_TOKEN_STREAM_H

#include "../core/autograd.h"

void* cpx_ai_token_stream_open(const char* path, i32 batch_size);
void* cpx_ai_token_stream_next(void* stream);
i32 cpx_ai_token_stream_sequence_length(void* stream);
i32 cpx_ai_token_stream_vocabulary_size(void* stream);
void cpx_ai_token_stream_close(void* stream);

void* cpx_ai_token_batch_inputs(void* batch, void* graph);
void* cpx_ai_token_batch_targets(void* batch, void* graph);
void cpx_ai_token_batch_destroy(void* batch);

#endif
