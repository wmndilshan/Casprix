/**
 * RNN Layer Implementation
 */

#include "rnn.h"
#include <stdlib.h>
#include <string.h>

RNNLayer* rnn_create(RNNType type, int input_size, int hidden_size,
                     int num_layers, bool bidirectional) {
    RNNLayer* rnn = (RNNLayer*)calloc(1, sizeof(RNNLayer));
    
    rnn->type = type;
    rnn->input_size = input_size;
    rnn->hidden_size = hidden_size;
    rnn->num_layers = num_layers;
    rnn->bidirectional = bidirectional;
    
    /* Allocate cells */
    rnn->cells = (void**)calloc(num_layers, sizeof(void*));
    if (bidirectional) {
        rnn->cells_backward = (void**)calloc(num_layers, sizeof(void*));
    }
    
    /* Create cells for each layer */
    for (int i = 0; i < num_layers; i++) {
        int layer_input_size = (i == 0) ? input_size : hidden_size;
        if (bidirectional && i > 0) {
            layer_input_size *= 2;  /* Concat forward + backward */
        }
        
        if (type == RNN_LSTM) {
            rnn->cells[i] = lstm_create(layer_input_size, hidden_size);
            if (bidirectional) {
                rnn->cells_backward[i] = lstm_create(layer_input_size, hidden_size);
            }
        } else if (type == RNN_GRU) {
            rnn->cells[i] = gru_create(layer_input_size, hidden_size);
            if (bidirectional) {
                rnn->cells_backward[i] = gru_create(layer_input_size, hidden_size);
            }
        }
    }
    
    return rnn;
}

void rnn_forward(RNNLayer* rnn, const Tensor* input,
                const Tensor* h0, const Tensor* c0,
                Tensor* output, Tensor* hn, Tensor* cn) {
    /* input: [seq_len, batch, input_size] */
    int seq_len = input->shape[0];
    int batch = input->shape[1];
    int hidden = rnn->hidden_size;
    int num_directions = rnn->bidirectional ? 2 : 1;
    
    /* Create temporary tensors for hidden/cell states */
    int state_shape[] = {batch, hidden};
    int input_shape[] = {batch, input->shape[2]};
    
    Tensor* h_prev = tensor_create(2, state_shape);
    Tensor* c_prev = rnn->type == RNN_LSTM ? tensor_create(2, state_shape) : NULL;
    Tensor* h_next = tensor_create(2, state_shape);
    Tensor* c_next = rnn->type == RNN_LSTM ? tensor_create(2, state_shape) : NULL;
    
    /* Process forward direction */
    for (int layer = 0; layer < rnn->num_layers; layer++) {
        /* Initialize hidden state */
        if (h0) {
            /* Extract h0 for this layer */
            int offset = layer * batch * hidden;
            memcpy(h_prev->data, h0->data + offset, batch * hidden * sizeof(float));
        } else {
            tensor_zeros(h_prev);
        }
        
        if (c0 && rnn->type == RNN_LSTM) {
            int offset = layer * batch * hidden;
            memcpy(c_prev->data, c0->data + offset, batch * hidden * sizeof(float));
        } else if (c_prev) {
            tensor_zeros(c_prev);
        }
        
        /* Process sequence */
        for (int t = 0; t < seq_len; t++) {
            /* Get input for this timestep */
            int input_offset = t * batch * input->shape[2];
            Tensor* step_input = tensor_create(2, input_shape);
            memcpy(step_input->data, input->data + input_offset,
                   batch * input->shape[2] * sizeof(float));
            
            /* Forward step */
            if (rnn->type == RNN_LSTM) {
                LSTMCell* cell = (LSTMCell*)rnn->cells[layer];
                lstm_forward(cell, step_input, h_prev, c_prev, h_next, c_next);
                tensor_copy(c_next, c_prev);
            } else {
                GRUCell* cell = (GRUCell*)rnn->cells[layer];
                gru_forward(cell, step_input, h_prev, h_next);
            }
            
            /* Copy output */
            int output_offset = t * batch * hidden;
            memcpy(output->data + output_offset, h_next->data, batch * hidden * sizeof(float));
            
            tensor_copy(h_next, h_prev);
            tensor_destroy(step_input);
        }
        
        /* Save final hidden state */
        if (hn) {
            int offset = layer * batch * hidden;
            memcpy(hn->data + offset, h_next->data, batch * hidden * sizeof(float));
        }
        
        if (cn && rnn->type == RNN_LSTM) {
            int offset = layer * batch * hidden;
            memcpy(cn->data + offset, c_next->data, batch * hidden * sizeof(float));
        }
    }
    
    /* Cleanup */
    tensor_destroy(h_prev);
    tensor_destroy(h_next);
    if (c_prev) tensor_destroy(c_prev);
    if (c_next) tensor_destroy(c_next);
}

void rnn_free(RNNLayer* rnn) {
    if (!rnn) return;
    
    for (int i = 0; i < rnn->num_layers;i++) {
        if (rnn->type == RNN_LSTM) {
            lstm_free((LSTMCell*)rnn->cells[i]);
            if (rnn->bidirectional && rnn->cells_backward) {
                lstm_free((LSTMCell*)rnn->cells_backward[i]);
            }
        } else {
            gru_free((GRUCell*)rnn->cells[i]);
            if (rnn->bidirectional && rnn->cells_backward) {
                gru_free((GRUCell*)rnn->cells_backward[i]);
            }
        }
    }
    
    free(rnn->cells);
    if (rnn->cells_backward) {
        free(rnn->cells_backward);
    }
    free(rnn);
}
