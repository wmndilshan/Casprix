/**
 * GRU Cell Implementation
 */

#include "gru.h"
#include "../activations/activations.h"
#include "../../llm/ops.h"
#include <stdlib.h>

GRUCell* gru_create(int input_size, int hidden_size) {
    GRUCell* cell = (GRUCell*)calloc(1, sizeof(GRUCell));
    
    cell->input_size = input_size;
    cell->hidden_size = hidden_size;
    
    /* Create gates */
    cell->Wr = dense_create(input_size, hidden_size, true);
    cell->Wz = dense_create(input_size, hidden_size, true);
    cell->Wh = dense_create(input_size, hidden_size, true);
    
    cell->Ur = dense_create(hidden_size, hidden_size, false);
    cell->Uz = dense_create(hidden_size, hidden_size, false);
    cell->Uh = dense_create(hidden_size, hidden_size, false);
    
    /* Initialize */
    dense_init_weights(cell->Wr, "xavier");
    dense_init_weights(cell->Wz, "xavier");
    dense_init_weights(cell->Wh, "xavier");
    dense_init_weights(cell->Ur, "xavier");
    dense_init_weights(cell->Uz, "xavier");
    dense_init_weights(cell->Uh, "xavier");
    
    return cell;
}

void gru_forward(GRUCell* cell, const Tensor* input,
                const Tensor* h_prev, Tensor* h_next) {
    int batch = input->shape[0];
    int hidden = cell->hidden_size;
    
    int gate_shape[] = {batch, hidden};
    Tensor* r_gate = tensor_create(2, gate_shape);
    Tensor* z_gate = tensor_create(2, gate_shape);
    Tensor* h_tilde = tensor_create(2, gate_shape);
    Tensor* temp1 = tensor_create(2, gate_shape);
    Tensor* temp2 = tensor_create(2, gate_shape);
    
    /* Reset gate: r = sigmoid(Wr * x + Ur * h_prev) */
    dense_forward(cell->Wr, input, temp1);
    dense_forward(cell->Ur, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, r_gate->data, batch * hidden);
    sigmoid_forward(r_gate, r_gate);
    
    /* Update gate: z = sigmoid(Wz * x + Uz * h_prev) */
    dense_forward(cell->Wz, input, temp1);
    dense_forward(cell->Uz, h_prev, temp2);
    vec_add_f32(temp1->data, temp2->data, z_gate->data, batch * hidden);
    sigmoid_forward(z_gate, z_gate);
    
    /* Candidate: h_tilde = tanh(Wh * x + Uh * (r * h_prev)) */
    vec_mul_f32(r_gate->data, h_prev->data, temp1->data, batch * hidden);
    dense_forward(cell->Wh, input, temp2);
    Tensor* r_h = tensor_create(2, gate_shape);
    tensor_copy(temp1, r_h);
    dense_forward(cell->Uh, r_h, temp1);
    vec_add_f32(temp2->data, temp1->data, h_tilde->data, batch * hidden);
    tanh_forward(h_tilde, h_tilde);
    
    /* New hidden: h_next = (1 - z) * h_prev + z * h_tilde */
    for (int i = 0; i < batch * hidden; i++) {
        h_next->data[i] = (1.0f - z_gate->data[i]) * h_prev->data[i] +
                         z_gate->data[i] * h_tilde->data[i];
    }
    
    /* Cleanup */
    tensor_destroy(r_gate);
    tensor_destroy(z_gate);
    tensor_destroy(h_tilde);
    tensor_destroy(temp1);
    tensor_destroy(temp2);
    tensor_destroy(r_h);
}

void gru_free(GRUCell* cell) {
    if (!cell) return;
    
    dense_free(cell->Wr);
    dense_free(cell->Wz);
    dense_free(cell->Wh);
    dense_free(cell->Ur);
    dense_free(cell->Uz);
    dense_free(cell->Uh);
    
    free(cell);
}
