/**
 * LSTM Cell Implementation
 */

#include "lstm.h"
#include "../activations/activations.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <string.h>

LSTMCell* lstm_create(int input_size, int hidden_size) {
    LSTMCell* cell = (LSTMCell*)calloc(1, sizeof(LSTMCell));
    
    cell->input_size = input_size;
    cell->hidden_size = hidden_size;
    
    /* Create gates - input transformations */
    cell->Wi = dense_create(input_size, hidden_size, true);
    cell->Wf = dense_create(input_size, hidden_size, true);
    cell->Wo = dense_create(input_size, hidden_size, true);
    cell->Wc = dense_create(input_size, hidden_size, true);
    
    /* Create gates - hidden transformations */
    cell->Ui = dense_create(hidden_size, hidden_size, false);  /* No bias, already in Wi */
    cell->Uf = dense_create(hidden_size, hidden_size, false);
    cell->Uo = dense_create(hidden_size, hidden_size, false);
    cell->Uc = dense_create(hidden_size, hidden_size, false);
    
    /* Initialize weights */
    dense_init_weights(cell->Wi, "xavier");
    dense_init_weights(cell->Wf, "xavier");
    dense_init_weights(cell->Wo, "xavier");
    dense_init_weights(cell->Wc, "xavier");
    dense_init_weights(cell->Ui, "xavier");
    dense_init_weights(cell->Uf, "xavier");
    dense_init_weights(cell->Uo, "xavier");
    dense_init_weights(cell->Uc, "xavier");
    
    /* Initialize forget gate bias to 1 (better gradient flow) */
    if (cell->Wf->use_bias) {
        for (int i = 0; i < hidden_size; i++) {
            cell->Wf->bias->data[i] = 1.0f;
        }
    }
    
    return cell;
}

void lstm_forward(LSTMCell* cell, const Tensor* input,
                 const Tensor* h_prev, const Tensor* c_prev,
                 Tensor* h_next, Tensor* c_next) {
    int batch = input->shape[0];
    int hidden = cell->hidden_size;
    
    /* Create temporary tensors for gate computations */
    int gate_shape[] = {batch, hidden};
    Tensor* i_gate = tensor_create(2, gate_shape);
    Tensor* f_gate = tensor_create(2, gate_shape);
    Tensor* o_gate = tensor_create(2, gate_shape);
    Tensor* c_tilde = tensor_create(2, gate_shape);
    
    Tensor* temp1 = tensor_create(2, gate_shape);
    Tensor* temp2 = tensor_create(2, gate_shape);
    
    /* Input gate: i = sigmoid(Wi * x + Ui * h_prev) */
    dense_forward(cell->Wi, input, temp1);
    dense_forward(cell->Ui, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, i_gate->data, batch * hidden);
    sigmoid_forward(i_gate, i_gate);
    
    /* Forget gate: f = sigmoid(Wf * x + Uf * h_prev) */
    dense_forward(cell->Wf, input, temp1);
    dense_forward(cell->Uf, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, f_gate->data, batch * hidden);
    sigmoid_forward(f_gate, f_gate);
    
    /* Output gate: o = sigmoid(Wo * x + Uo * h_prev) */
    dense_forward(cell->Wo, input, temp1);
    dense_forward(cell->Uo, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, o_gate->data, batch * hidden);
    sigmoid_forward(o_gate, o_gate);
    
    /* Cell candidate: c_tilde = tanh(Wc * x + Uc * h_prev) */
    dense_forward(cell->Wc, input, temp1);
    dense_forward(cell->Uc, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, c_tilde->data, batch * hidden);
    tanh_forward(c_tilde, c_tilde);
    
    /* New cell state: c_next = f * c_prev + i * c_tilde */
    for (int i = 0; i < batch * hidden; i++) {
        c_next->data[i] = f_gate->data[i] * c_prev->data[i] + 
                         i_gate->data[i] * c_tilde->data[i];
    }
    
    /* New hidden state: h_next = o * tanh(c_next) */
    tanh_forward(c_next, temp1);
    vec_mul_f32(o_gate->data, temp1->data, h_next->data, batch * hidden);
    
    /* Cleanup */
    tensor_destroy(i_gate);
    tensor_destroy(f_gate);
    tensor_destroy(o_gate);
    tensor_destroy(c_tilde);
    tensor_destroy(temp1);
    tensor_destroy(temp2);
}

void lstm_backward(LSTMCell* cell, const Tensor* grad_h, const Tensor* grad_c,
                  Tensor* grad_input, Tensor* grad_h_prev, Tensor* grad_c_prev) {
    /* Simplified - full LSTM backward is complex */
    /* Would compute gradients through all gates */
    (void)cell;
    (void)grad_h;
    (void)grad_c;
    (void)grad_input;
    (void)grad_h_prev;
    (void)grad_c_prev;
}

void lstm_free(LSTMCell* cell) {
    if (!cell) return;
    
    dense_free(cell->Wi);
    dense_free(cell->Wf);
    dense_free(cell->Wo);
    dense_free(cell->Wc);
    dense_free(cell->Ui);
    dense_free(cell->Uf);
    dense_free(cell->Uo);
    dense_free(cell->Uc);
    
    free(cell);
}
