#include "../../runtime/ai/core/tensor.h"
#include "../../runtime/ai/distributed/cpu_collective.h"
#include "../../runtime/ai/classical/algorithms.h"
#include "../../runtime/ai/evaluation/metrics.h"
#include "../../runtime/ai/training/training.h"
#include "../../runtime/ai/training/optimizer.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); return 1; } \
} while (0)

static Tensor* make_tensor(i32 ndim, const i32* shape, const float* data) {
    return tensor_create_from_data(ndim, shape, data);
}

static int test_metrics(void) {
    const float prediction[] = { 1, 2, 4, 8 };
    const float target[] = { 1, 3, 5, 7 };
    CHECK(fabsf(metrics_mean_squared_error(prediction, target, 4) - 0.75f) < 1e-6f,
          "MSE is correct");
    CHECK(fabsf(metrics_mean_absolute_error(prediction, target, 4) - 0.75f) < 1e-6f,
          "MAE is correct");
    RunningMetric running;
    metrics_running_reset(&running);
    metrics_running_update(&running, 2.0, 2.0);
    metrics_running_update(&running, 8.0, 1.0);
    CHECK(fabs(metrics_running_value(&running) - 4.0) < 1e-12, "weighted running mean");
    return 0;
}

static int test_collectives_and_training(void) {
    i32 shape[] = { 2 };
    float first_data[] = { 1.0f, 3.0f }, second_data[] = { 3.0f, 5.0f };
    Tensor* first = make_tensor(1, shape, first_data);
    Tensor* second = make_tensor(1, shape, second_data);
    const Tensor* workers[] = { first, second };
    Tensor* reduced = tensor_create(1, shape);
    AiCpuProcessGroup group;
    CHECK(ai_cpu_process_group_init(&group, 2) == AI_COLLECTIVE_OK, "process group init");
    CHECK(ai_cpu_allreduce(&group, workers, reduced, AI_REDUCE_MEAN) == AI_COLLECTIVE_OK,
          "CPU mean all-reduce");
    CHECK(reduced->data[0] == 2.0f && reduced->data[1] == 4.0f, "all-reduce values");
    second->data[1] = NAN;
    CHECK(ai_cpu_allreduce(&group, workers, reduced, AI_REDUCE_MEAN) == AI_COLLECTIVE_NONFINITE,
          "all-reduce rejects non-finite gradients");
    CHECK(reduced->data[0] == 2.0f && reduced->data[1] == 4.0f,
          "failed collective is atomic");
    second->data[1] = 5.0f;

    Tensor* gradient[] = { reduced };
    float parameter_data[] = { 5.0f, -2.0f };
    Tensor* parameter_value = make_tensor(1, shape, parameter_data);
    Tensor* parameters[] = { parameter_value };
    float norm;
    CHECK(ai_clip_grad_norm(gradient, 1, 1.0f, &norm) == AI_TRAIN_OK && norm > 4.0f,
          "global gradient clipping");
    CHECK(ai_sgd_step(parameters, gradient, 1, 0.1f, 0.0f) == AI_TRAIN_OK, "SGD update");
    CHECK(isfinite(parameter_value->data[0]), "finite parameter after update");
    tensor_destroy(first); tensor_destroy(second); tensor_destroy(reduced); tensor_destroy(parameter_value);
    return 0;
}

static int test_distributed_linear_training(void) {
    i32 scalar_shape[] = { 1 };
    Tensor* gradients[2][2];
    Tensor** worker_gradients[] = { gradients[0], gradients[1] };
    for (int worker = 0; worker < 2; worker++)
        for (int parameter = 0; parameter < 2; parameter++)
            gradients[worker][parameter] = tensor_create(1, scalar_shape);
    Tensor* weight = tensor_create(1, scalar_shape);
    Tensor* bias = tensor_create(1, scalar_shape);
    Tensor* parameters[] = { weight, bias };
    AiCpuProcessGroup group;
    ai_cpu_process_group_init(&group, 2);
    const float x[] = { -1.0f, 0.0f, 1.0f, 2.0f };
    for (int epoch = 0; epoch < 500; epoch++) {
        for (int worker = 0; worker < 2; worker++) {
            float dw = 0.0f, db = 0.0f;
            for (int local = 0; local < 2; local++) {
                int sample = worker * 2 + local;
                float error = weight->data[0] * x[sample] + bias->data[0] - (3.0f * x[sample] + 2.0f);
                dw += error * x[sample]; db += error;
            }
            gradients[worker][0]->data[0] = dw; /* local mean gradient including MSE factor 2 */
            gradients[worker][1]->data[0] = db;
        }
        CHECK(ai_cpu_sync_gradients(&group, worker_gradients, 2, AI_REDUCE_MEAN) ==
              AI_COLLECTIVE_OK, "synchronize worker gradients");
        Tensor* mean_gradients[] = { gradients[0][0], gradients[0][1] };
        CHECK(ai_sgd_step(parameters, mean_gradients, 2, 0.05f, 0.0f) == AI_TRAIN_OK,
              "distributed SGD step");
    }
    CHECK(fabsf(weight->data[0] - 3.0f) < 1e-3f && fabsf(bias->data[0] - 2.0f) < 1e-3f,
          "two CPU workers learn y=3x+2");
    for (int worker = 0; worker < 2; worker++)
        for (int parameter = 0; parameter < 2; parameter++) tensor_destroy(gradients[worker][parameter]);
    tensor_destroy(weight); tensor_destroy(bias);
    return 0;
}

static int test_shared_optimizers(void) {
    const AiOptimizerKind kinds[] = { AI_OPTIMIZER_SGD, AI_OPTIMIZER_ADAM,
                                      AI_OPTIMIZER_RMSPROP, AI_OPTIMIZER_ADAGRAD };
    const float rates[] = { 0.1f, 0.1f, 0.05f, 0.5f };
    for (int kind = 0; kind < 4; kind++) {
        i32 shape[] = { 1 };
        Tensor* parameter = tensor_create(1, shape);
        Tensor* gradient = tensor_create(1, shape);
        Tensor* parameters[] = { parameter };
        Tensor* gradients[] = { gradient };
        AiOptimizerConfig config = ai_optimizer_default_config(kinds[kind], rates[kind]);
        AiOptimizer* optimizer = ai_optimizer_create(kinds[kind], 1, config);
        CHECK(optimizer && ai_optimizer_init(optimizer, parameters) == AI_TRAIN_OK,
              "initialize shared optimizer");
        for (int step = 0; step < 600; step++) {
            gradient->data[0] = 2.0f * (parameter->data[0] - 3.0f);
            AiTrainStatus status = ai_optimizer_step(optimizer, parameters, gradients);
            if (status != AI_TRAIN_OK) {
                fprintf(stderr, "optimizer kind %d failed at step %d with status %d\n",
                        kind, step, status);
                CHECK(false, "shared optimizer step");
            }
        }
        CHECK(fabsf(parameter->data[0] - 3.0f) < 0.05f,
              "shared optimizer converges");
        ai_optimizer_destroy(optimizer);
        tensor_destroy(gradient); tensor_destroy(parameter);
    }
    return 0;
}

static int test_algorithms(void) {
    i32 x_shape[] = { 6, 2 }, y_shape[] = { 6 };
    float x_data[] = { -1,0, 0,-1, 0,0, 1,0, 0,1, 1,1 };
    float y_data[] = { -1,-4,1,3,6,8 }; /* y = 2*x0 + 5*x1 + 1 */
    Tensor* x = make_tensor(2, x_shape, x_data); Tensor* y = make_tensor(1, y_shape, y_data);
    Tensor* predictions = tensor_create(1, y_shape);
    AiLinearRegression* linear = ai_linear_regression_create(2);
    CHECK(ai_linear_regression_fit(linear, x, y, 0.05f, 1000) == AI_ML_OK, "fit linear model");
    ai_linear_regression_predict(linear, x, predictions);
    CHECK(metrics_r2_score(predictions->data, y->data, 6) > 0.999f, "linear regression R2");

    float labels_data[] = { 0,0,0,1,1,1 };
    Tensor* labels = make_tensor(1, y_shape, labels_data);
    AiLogisticRegression* logistic = ai_logistic_regression_create(2);
    CHECK(ai_logistic_regression_fit(logistic, x, labels, 0.2f, 1000, 0.001f) == AI_ML_OK,
          "fit logistic model");
    ai_logistic_regression_predict_proba(logistic, x, predictions);
    int correct = 0;
    for (int i = 0; i < 6; i++) if ((predictions->data[i] >= 0.5f) == (labels_data[i] > 0.5f)) correct++;
    CHECK(correct == 6, "logistic classification accuracy");

    i32 cluster_shape[] = { 6, 2 };
    float cluster_data[] = { 0,0, 0.2f,-0.1f, -0.2f,0.1f, 10,10, 9.8f,10.1f, 10.2f,9.9f };
    Tensor* points = make_tensor(2, cluster_shape, cluster_data);
    AiKMeans* kmeans = ai_kmeans_create(2, 2);
    CHECK(ai_kmeans_fit(kmeans, points, 50, 1e-5f) == AI_ML_OK, "fit k-means");
    CHECK(ai_kmeans_predict(kmeans, points->data) != ai_kmeans_predict(kmeans, points->data + 6),
          "k-means separates clusters");
    CHECK(kmeans->inertia < 1.0f, "k-means low inertia");

    ai_kmeans_destroy(kmeans); tensor_destroy(points);
    ai_logistic_regression_destroy(logistic); tensor_destroy(labels);
    ai_linear_regression_destroy(linear); tensor_destroy(predictions); tensor_destroy(y); tensor_destroy(x);
    return 0;
}

static int test_real_world_iris_example(void) {
    /* Fisher's Iris measurements: petal length/width, binary subset
     * (Iris setosa=0, Iris versicolor=1). Four held-out flowers are not
     * present in the training tensor. */
    i32 train_shape[] = { 16, 2 }, label_shape[] = { 16 };
    float train_data[] = {
        1.4f,.2f, 1.3f,.2f, 1.5f,.2f, 1.4f,.3f,
        1.7f,.4f, 1.4f,.3f, 1.5f,.1f, 1.5f,.2f,
        4.7f,1.4f, 4.5f,1.5f, 4.9f,1.5f, 4.0f,1.3f,
        4.6f,1.5f, 4.5f,1.3f, 4.7f,1.6f, 3.3f,1.0f
    };
    float label_data[] = { 0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1 };
    Tensor* train = make_tensor(2, train_shape, train_data);
    Tensor* labels = make_tensor(1, label_shape, label_data);
    AiLogisticRegression* model = ai_logistic_regression_create(2);
    CHECK(ai_logistic_regression_fit(model, train, labels, 0.1f, 1500, 0.001f) == AI_ML_OK,
          "train on real Iris measurements");

    i32 test_shape[] = { 4, 2 }, output_shape[] = { 4 };
    float test_data[] = { 1.6f,.2f, 1.4f,.2f, 4.2f,1.3f, 4.2f,1.2f };
    Tensor* held_out = make_tensor(2, test_shape, test_data);
    Tensor* probabilities = tensor_create(1, output_shape);
    ai_logistic_regression_predict_proba(model, held_out, probabilities);
    float predicted[] = { 0,0,0,0 }, expected[] = { 0,0,1,1 };
    for (int i = 0; i < 4; i++) predicted[i] = probabilities->data[i] >= 0.5f ? 1.0f : 0.0f;
    CHECK(metrics_accuracy(predicted, expected, 4) == 1.0f,
          "classify held-out Iris flowers");

    tensor_destroy(probabilities); tensor_destroy(held_out);
    ai_logistic_regression_destroy(model); tensor_destroy(labels); tensor_destroy(train);
    return 0;
}

int main(void) {
    CHECK(test_metrics() == 0, "metrics suite");
    CHECK(test_collectives_and_training() == 0, "collective/training suite");
    CHECK(test_distributed_linear_training() == 0, "distributed real training suite");
    CHECK(test_shared_optimizers() == 0, "shared optimizer suite");
    CHECK(test_algorithms() == 0, "classical ML suite");
    CHECK(test_real_world_iris_example() == 0, "real-world Iris suite");
    puts("AI CPU training, collectives, metrics, and ML tests passed");
    return 0;
}
