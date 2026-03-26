/**
 * GRU (Gated Recurrent Unit) Cell
 * 
 * Simpler alternative to LSTM with reset and update gates
 */

#ifndef NN_GRU_H
#define NN_GRU_H

#include "../../llm/tensor.h"
#include "dense.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Gates */
    DenseLayer* Wr;  /* Reset gate - input */
    DenseLayer* Wz;  /* Update gate - input */
    DenseLayer* Wh;  /* Candidate hidden - input */
    
    DenseLayer* Ur;  /* Reset gate - hidden */
    DenseLayer* Uz;  /* Update gate - hidden */
    DenseLayer* Uh;  /* Candidate hidden - hidden */
    
    int input_size;
    int hidden_size;
} GRUCell;

/**
 * Create GRU cell
 */
GRUCell* gru_create(int input_size, int hidden_size);

/**
 * Forward pass
 * @param input Input tensor [batch, input_size]
 * @param h_prev Previous hidden state [batch, hidden_size]
 * @param h_next Output hidden state [batch, hidden_size] (allocated by caller)
 */
void gru_forward(GRUCell* cell, const Tensor* input,
                const Tensor* h_prev, Tensor* h_next);

/**
 * Free cell
 */
void gru_free(GRUCell* cell);

#ifdef __cplusplus
}
#endif

#endif /* NN_GRU_H */
