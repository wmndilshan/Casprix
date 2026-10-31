#include "autograd.h"
#include <math.h>
#include <stdio.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

static int test_shared_branch_gradient(void) {
    AiGraph* graph = ai_graph_create();
    AiValue* x = ai_parameter(graph, 1, (i32[]){1}, (f32[]){3.0f});
    AiValue* square = ai_mul(x, x);
    AiValue* y = ai_add(square, x);
    AiValue* loss = ai_sum(y);
    AiBackwardGraph backward = {0};

    CHECK(ai_build_backward_graph(loss, &backward), "build backward graph");
    CHECK(backward.count == 3, "topological graph contains each reachable op once");
    ai_backward_graph_destroy(&backward);
    CHECK(ai_backward(loss), "run reverse topological backward pass");
    CHECK(fabsf(x->grad->data[0] - 7.0f) < 1e-5f, "shared branch gradient is accumulated");
    CHECK(ai_backward(loss), "repeat backward pass");
    CHECK(fabsf(x->grad->data[0] - 14.0f) < 1e-5f,
          "leaf gradients accumulate without reusing stale intermediate gradients");
    ai_graph_destroy(graph);
    return 0;
}

static int test_view_lifetime(void) {
    Tensor* parent = tensor_create_from_data(2, (i32[]){2, 2},
                                              (f32[]){1, 2, 3, 4});
    Tensor* row = tensor_slice_view(parent, 0, 1, 1);
    Tensor* column = tensor_slice_view(parent, 1, 1, 1);
    Tensor* packed_column = tensor_clone(column);
    CHECK(row != NULL, "create ref-counted tensor slice");
    CHECK(column && packed_column, "create and copy strided tensor slice");
    CHECK(packed_column->data[0] == 2.0f && packed_column->data[1] == 4.0f,
          "strided views copy by logical tensor order");
    tensor_destroy(parent);
    CHECK(row->data[0] == 3.0f && row->data[1] == 4.0f,
          "view retains storage after parent descriptor is destroyed");
    tensor_destroy(row);
    tensor_destroy(column);
    tensor_destroy(packed_column);
    return 0;
}

static int test_linear_regression(void) {
    const f32 x_data[] = {-2, -1, 0, 1, 2};
    const f32 y_data[] = {-4, -1, 2, 5, 8}; /* y = 3x + 2 */
    AiGraph* graph = ai_graph_create();
    AiValue* x = ai_value_copy(graph, 2, (i32[]){5, 1}, x_data, false);
    AiValue* target = ai_value_copy(graph, 2, (i32[]){5, 1}, y_data, false);
    AiValue* weight = ai_parameter(graph, 2, (i32[]){1, 1}, (f32[]){0});
    AiValue* bias = ai_parameter(graph, 1, (i32[]){1}, (f32[]){0});
    AiValue* params[] = {weight, bias};
    f32 first_loss = 0, final_loss = 0;

    for (int epoch = 0; epoch < 160; epoch++) {
        ai_graph_next_step(graph);
        AiValue* prediction = ai_add(ai_matmul(x, weight), bias);
        AiValue* loss = ai_mse_loss(prediction, target);
        CHECK(loss && ai_backward(loss), "linear regression forward/backward");
        if (epoch == 0) first_loss = loss->data->data[0];
        final_loss = loss->data->data[0];
        ai_graph_sgd_step(params, 2, 0.08f);
    }

    printf("linear regression: loss %.6f -> %.6f, w=%.4f b=%.4f\n",
           first_loss, final_loss, weight->data->data[0], bias->data->data[0]);
    CHECK(final_loss < first_loss * 0.001f, "training reduces real loss");
    CHECK(fabsf(weight->data->data[0] - 3.0f) < 0.02f, "learned slope");
    CHECK(fabsf(bias->data->data[0] - 2.0f) < 0.02f, "learned bias");
    ai_graph_destroy(graph);
    return 0;
}

static int test_decoder_primitives(void) {
    AiGraph* graph = ai_graph_create();
    AiValue* ids = ai_value_copy(graph, 2, (i32[]){3, 1}, (f32[]){0, 1, 2}, false);
    AiValue* targets = ai_value_copy(graph, 2, (i32[]){3, 1}, (f32[]){1, 2, 3}, false);
    AiValue* table = ai_parameter(graph, 2, (i32[]){4, 4},
        (f32[]){.1f,.2f,.3f,.4f, .2f,.1f,.4f,.3f,
                .4f,.3f,.2f,.1f, .3f,.4f,.1f,.2f});
    AiValue* gamma = ai_parameter(graph, 2, (i32[]){1, 4},
                                  (f32[]){1, 1, 1, 1});
    AiValue* beta = ai_parameter(graph, 2, (i32[]){1, 4},
                                 (f32[]){0, 0, 0, 0});
    AiValue* projection = ai_parameter(graph, 2, (i32[]){4, 4},
        (f32[]){.3f,.1f,.2f,.4f, .4f,.2f,.1f,.3f,
                .1f,.4f,.3f,.2f, .2f,.3f,.4f,.1f});

    AiValue* embedded = ai_embedding(ids, table);
    AiValue* attended = ai_causal_attention(embedded, embedded, embedded, 2, 3);
    AiValue* normalized = ai_layer_norm(attended, gamma, beta, 1e-5f);
    AiValue* logits = ai_matmul(normalized, projection);
    AiValue* loss = ai_cross_entropy(logits, targets);
    CHECK(loss && isfinite(loss->data->data[0]), "decoder primitive forward pass");
    CHECK(ai_backward(loss), "decoder primitive topological backward pass");
    CHECK(table->grad && projection->grad && gamma->grad && beta->grad,
          "all decoder parameters receive gradients");
    f32 magnitude = 0.0f;
    for (size_t i = 0; i < table->grad->size; ++i) {
        CHECK(isfinite(table->grad->data[i]), "embedding gradient is finite");
        magnitude += fabsf(table->grad->data[i]);
    }
    CHECK(magnitude > 0.0f, "embedding gradient propagates through attention");
    ai_graph_destroy(graph);
    return 0;
}

int main(void) {
    CHECK(test_shared_branch_gradient() == 0, "branch test");
    CHECK(test_view_lifetime() == 0, "storage test");
    CHECK(test_linear_regression() == 0, "training test");
    CHECK(test_decoder_primitives() == 0, "decoder primitive test");
    puts("AI core autograd tests passed");
    return 0;
}
