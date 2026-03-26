/**
 * RNN Layer
 * 
 * Wrapper for LSTM/GRU cells to process sequences
 */

#ifndef NN_RNN_H
#define NN_RNN_H

#include "../../llm/tensor.h"
#include "../layers/lstm.h"
#include "../layers/gru.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RNN_LSTM,
    RNN_GRU
} RNNType;

typedef struct {
    RNNType type;
    int input_size;
    int hidden_size;
    int num_layers;
    bool bidirectional;
    
    /* Layers (array of cells) */
    void** cells;          /* Array of LSTMCell* or GRUCell* */
    void** cells_backward; /* For bidirectional */
} RNNLayer;

/**
 * Create RNN layer
 * @param type RNN type (LSTM or GRU)
 * @param input_size Input feature size
 * @param hidden_size Hidden state size
 * @param num_layers Number of stacked layers
 * @param bidirectional Whether to use bidirectional RNN
 */
RNNLayer* rnn_create(RNNType type, int input_size, int hidden_size,
                     int num_layers, bool bidirectional);

/**
 * Forward pass through sequence
 * @param input Input sequence [seq_len, batch, input_size]
 * @param h0 Initial hidden state [num_layers * num_directions, batch, hidden_size] (can be NULL)
 * @param c0 Initial cell state for LSTM (can be NULL)
 * @param output Output sequence [seq_len, batch, hidden_size * num_directions]
 * @param hn Final hidden state (same shape as h0)
 * @param cn Final cell state for LSTM
 */
void rnn_forward(RNNLayer* rnn, const Tensor* input,
                const Tensor* h0, const Tensor* c0,
                Tensor* output, Tensor* hn, Tensor* cn);

/**
 * Free RNN layer
 */
void rnn_free(RNNLayer* rnn);

#ifdef __cplusplus
}
#endif

#endif /* NN_RNN_H */
