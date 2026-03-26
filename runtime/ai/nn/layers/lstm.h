/**
 * LSTM (Long Short-Term Memory) Cell
 * 
 * Implements LSTM with forget gate, input gate, output gate, and cell state
 */

#ifndef NN_LSTM_H
#define NN_LSTM_H

#include "../../llm/tensor.h"
#include "dense.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Gates */
    DenseLayer* Wi;  /* Input gate - input transformation */
    DenseLayer* Wf;  /* Forget gate - input transformation */
    DenseLayer* Wo;  /* Output gate - input transformation */
    DenseLayer* Wc;  /* Cell gate - input transformation */
    
    DenseLayer* Ui;  /* Input gate - hidden transformation */
    DenseLayer* Uf;  /* Forget gate - hidden transformation */
    DenseLayer* Uo;  /* Output gate - hidden transformation */
    DenseLayer* Uc;  /* Cell gate - hidden transformation */
    
    int input_size;
    int hidden_size;
    
    /* Cached for backward */
    Tensor* input_cache;
    Tensor* h_prev_cache;
    Tensor* c_prev_cache;
    Tensor* gates_cache;  /* Store gate activations */
} LSTMCell;

/**
 * Create LSTM cell
 */
LSTMCell* lstm_create(int input_size, int hidden_size);

/**
 * Forward pass
 * @param input Input tensor [batch, input_size]
 * @param h_prev Previous hidden state [batch, hidden_size]
 * @param c_prev Previous cell state [batch, hidden_size]
 * @param h_next Output hidden state [batch, hidden_size] (allocated by caller)
 * @param c_next Output cell state [batch, hidden_size] (allocated by caller)
 */
void lstm_forward(LSTMCell* cell, const Tensor* input,
                 const Tensor* h_prev, const Tensor* c_prev,
                 Tensor* h_next, Tensor* c_next);

/**
 * Backward pass (simplified)
 */
void lstm_backward(LSTMCell* cell, const Tensor* grad_h, const Tensor* grad_c,
                  Tensor* grad_input, Tensor* grad_h_prev, Tensor* grad_c_prev);

/**
 * Free cell
 */
void lstm_free(LSTMCell* cell);

#ifdef __cplusplus
}
#endif

#endif /* NN_LSTM_H */
