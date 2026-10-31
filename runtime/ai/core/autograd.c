#include "autograd.h"
#include "../kernels/cpu_ops.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AiNode {
    AiOp op;
    AiValue* inputs[3];
    size_t input_count;
    AiValue* output;
    size_t id;
    void* auxiliary;
    i32 option;
    i32 option2;
    f32 epsilon;
};

struct AiGraph {
    AiValue** values;
    size_t value_count, value_capacity;
    AiNode** nodes;
    size_t node_count, node_capacity;
    char error[192];
};

static void set_error(AiGraph* graph, const char* message) {
    if (!graph) return;
    snprintf(graph->error, sizeof(graph->error), "%s", message ? message : "AI runtime error");
}

static bool grow(void*** items, size_t* capacity, size_t needed) {
    if (needed <= *capacity) return true;
    size_t next = *capacity ? *capacity * 2 : 16;
    while (next < needed) next *= 2;
    void** resized = (void**)realloc(*items, next * sizeof(void*));
    if (!resized) return false;
    *items = resized; *capacity = next;
    return true;
}

AiGraph* ai_graph_create(void) { return (AiGraph*)calloc(1, sizeof(AiGraph)); }

void ai_graph_destroy(AiGraph* graph) {
    if (!graph) return;
    for (size_t i = 0; i < graph->node_count; i++) {
        free(graph->nodes[i]->auxiliary);
        free(graph->nodes[i]);
    }
    for (size_t i = 0; i < graph->value_count; i++) {
        AiValue* value = graph->values[i];
        tensor_destroy(value->grad);
        if (value->owns_data) tensor_destroy(value->data);
        free(value);
    }
    free(graph->nodes); free(graph->values); free(graph);
}

const char* ai_graph_error(const AiGraph* graph) {
    return graph && graph->error[0] ? graph->error : NULL;
}

void ai_graph_next_step(AiGraph* graph) {
    if (!graph) return;
    for (size_t i = 0; i < graph->node_count; i++) {
        free(graph->nodes[i]->auxiliary);
        free(graph->nodes[i]);
    }
    graph->node_count = 0;

    size_t kept = 0;
    for (size_t i = 0; i < graph->value_count; i++) {
        AiValue* value = graph->values[i];
        if (!value->creator && value->persistent) {
            if (value->grad) tensor_zeros(value->grad);
            value->id = kept;
            graph->values[kept++] = value;
            continue;
        }
        tensor_destroy(value->grad);
        if (value->owns_data) tensor_destroy(value->data);
        free(value);
    }
    graph->value_count = kept;
    graph->error[0] = '\0';
}

static AiValue* register_value(AiGraph* graph, Tensor* tensor,
                               bool requires_grad, bool owns_data) {
    if (!graph || !tensor || !grow((void***)&graph->values, &graph->value_capacity,
                                    graph->value_count + 1)) {
        set_error(graph, "failed to allocate graph value");
        return NULL;
    }
    AiValue* value = (AiValue*)calloc(1, sizeof(AiValue));
    if (!value) { set_error(graph, "failed to allocate graph value"); return NULL; }
    value->data = tensor; value->graph = graph; value->requires_grad = requires_grad;
    value->owns_data = owns_data; value->persistent = true; value->id = graph->value_count;
    graph->values[graph->value_count++] = value;
    return value;
}

AiValue* ai_value(AiGraph* graph, Tensor* tensor, bool requires_grad) {
    return register_value(graph, tensor, requires_grad, false);
}

AiValue* ai_value_copy(AiGraph* graph, i32 ndim, const i32* shape,
                       const f32* data, bool requires_grad) {
    Tensor* tensor = tensor_create_from_data(ndim, shape, data);
    if (!tensor) { set_error(graph, "invalid tensor shape or allocation failure"); return NULL; }
    AiValue* value = register_value(graph, tensor, requires_grad, true);
    if (!value) tensor_destroy(tensor);
    return value;
}

AiValue* ai_value_copy_ephemeral(AiGraph* graph, i32 ndim, const i32* shape,
                                 const f32* data) {
    AiValue* value = ai_value_copy(graph, ndim, shape, data, false);
    if (value) value->persistent = false;
    return value;
}

AiValue* ai_parameter(AiGraph* graph, i32 ndim, const i32* shape,
                      const f32* initial_data) {
    return ai_value_copy(graph, ndim, shape, initial_data, true);
}

static bool compatible(AiValue* a, AiValue* b) {
    if (!a || !b || a->graph != b->graph) {
        if (a) set_error(a->graph, "operation inputs must belong to the same graph");
        return false;
    }
    return true;
}

static AiValue* record(AiGraph* graph, AiOp op, AiValue* a, AiValue* b,
                       Tensor* output) {
    bool requires = (a && a->requires_grad) || (b && b->requires_grad);
    AiValue* value = register_value(graph, output, requires, true);
    if (!value) { tensor_destroy(output); return NULL; }
    if (!requires) return value;
    if (!grow((void***)&graph->nodes, &graph->node_capacity, graph->node_count + 1)) {
        set_error(graph, "failed to allocate operation node"); return NULL;
    }
    AiNode* node = (AiNode*)calloc(1, sizeof(AiNode));
    if (!node) { set_error(graph, "failed to allocate operation node"); return NULL; }
    node->op = op; node->inputs[0] = a; node->inputs[1] = b;
    node->input_count = b ? 2 : 1; node->output = value; node->id = graph->node_count;
    value->creator = node;
    graph->nodes[graph->node_count++] = node;
    return value;
}

static AiValue* record_three(AiGraph* graph, AiOp op, AiValue* a, AiValue* b,
                             AiValue* c, Tensor* output, void* auxiliary,
                             i32 option, f32 epsilon) {
    bool requires = (a && a->requires_grad) || (b && b->requires_grad) ||
                    (c && c->requires_grad);
    AiValue* result = register_value(graph, output, requires, true);
    if (!result) { tensor_destroy(output); free(auxiliary); return NULL; }
    if (!requires) { free(auxiliary); return result; }
    if (!grow((void***)&graph->nodes, &graph->node_capacity,
              graph->node_count + 1)) {
        set_error(graph, "failed to allocate operation node");
        free(auxiliary); return NULL;
    }
    AiNode* node = (AiNode*)calloc(1, sizeof(AiNode));
    if (!node) { set_error(graph, "failed to allocate operation node"); free(auxiliary); return NULL; }
    node->op = op; node->inputs[0] = a; node->inputs[1] = b; node->inputs[2] = c;
    node->input_count = c ? 3 : (b ? 2 : 1); node->output = result;
    node->id = graph->node_count; node->auxiliary = auxiliary;
    node->option = option; node->epsilon = epsilon;
    result->creator = node; graph->nodes[graph->node_count++] = node;
    return result;
}

static AiValue* binary(AiValue* a, AiValue* b, AiOp op) {
    if (!compatible(a, b)) return NULL;
    size_t size = a->data->size > b->data->size ? a->data->size : b->data->size;
    if (a->data->size != b->data->size && a->data->size != 1 && b->data->size != 1) {
        set_error(a->graph, "binary operation requires equal shapes or a scalar operand");
        return NULL;
    }
    Tensor* shape_source = a->data->size == size ? a->data : b->data;
    Tensor* out = tensor_create(shape_source->ndim, shape_source->shape);
    if (!out) { set_error(a->graph, "failed to allocate operation output"); return NULL; }
    for (size_t i = 0; i < size; i++) {
        f32 av = a->data->data[a->data->size == 1 ? 0 : i];
        f32 bv = b->data->data[b->data->size == 1 ? 0 : i];
        out->data[i] = op == AI_OP_ADD ? av + bv : op == AI_OP_SUB ? av - bv : av * bv;
    }
    return record(a->graph, op, a, b, out);
}

AiValue* ai_add(AiValue* a, AiValue* b) { return binary(a, b, AI_OP_ADD); }
AiValue* ai_sub(AiValue* a, AiValue* b) { return binary(a, b, AI_OP_SUB); }
AiValue* ai_mul(AiValue* a, AiValue* b) { return binary(a, b, AI_OP_MUL); }

AiValue* ai_matmul(AiValue* a, AiValue* b) {
    if (!compatible(a, b)) return NULL;
    if (a->data->ndim != 2 || b->data->ndim != 2 || a->data->shape[1] != b->data->shape[0]) {
        set_error(a->graph, "matmul requires [M,K] and [K,N] tensors"); return NULL;
    }
    i32 m = a->data->shape[0], k = a->data->shape[1], n = b->data->shape[1];
    Tensor* out = tensor_create(2, (i32[]){m, n});
    if (!out) return NULL;
    gemm_f32(a->data->data, b->data->data, out->data, m, k, n);
    return record(a->graph, AI_OP_MATMUL, a, b, out);
}

static AiValue* unary(AiValue* x, AiOp op) {
    if (!x) return NULL;
    Tensor* out = tensor_create(x->data->ndim, x->data->shape);
    if (!out) return NULL;
    for (size_t i = 0; i < x->data->size; i++) {
        f32 v = x->data->data[i];
        out->data[i] = op == AI_OP_RELU ? (v > 0 ? v : 0) : 1.0f / (1.0f + expf(-v));
    }
    return record(x->graph, op, x, NULL, out);
}

AiValue* ai_relu(AiValue* x) { return unary(x, AI_OP_RELU); }
AiValue* ai_sigmoid(AiValue* x) { return unary(x, AI_OP_SIGMOID); }

static AiValue* reduce(AiValue* x, AiOp op) {
    if (!x) return NULL;
    Tensor* out = tensor_create(1, (i32[]){1});
    if (!out) return NULL;
    f32 sum = 0;
    for (size_t i = 0; i < x->data->size; i++) sum += x->data->data[i];
    out->data[0] = op == AI_OP_MEAN ? sum / (f32)x->data->size : sum;
    return record(x->graph, op, x, NULL, out);
}

AiValue* ai_sum(AiValue* x) { return reduce(x, AI_OP_SUM); }
AiValue* ai_mean(AiValue* x) { return reduce(x, AI_OP_MEAN); }

AiValue* ai_mse_loss(AiValue* prediction, AiValue* target) {
    AiValue* error = ai_sub(prediction, target);
    return error ? ai_mean(ai_mul(error, error)) : NULL;
}

AiValue* ai_embedding(AiValue* token_ids, AiValue* table) {
    if (!compatible(token_ids, table) || token_ids->data->ndim != 2 ||
        token_ids->data->shape[1] != 1 || table->data->ndim != 2) {
        if (token_ids) set_error(token_ids->graph, "embedding requires token ids [N,1] and table [V,D]");
        return NULL;
    }
    i32 count = token_ids->data->shape[0], vocab = table->data->shape[0];
    i32 width = table->data->shape[1];
    Tensor* out = tensor_create(2, (i32[]){count, width});
    if (!out) return NULL;
    for (i32 row = 0; row < count; ++row) {
        i32 token = (i32)token_ids->data->data[row];
        if (token < 0 || token >= vocab) {
            tensor_destroy(out); set_error(token_ids->graph, "embedding token id is outside the vocabulary");
            return NULL;
        }
        memcpy(out->data + (size_t)row * width,
               table->data->data + (size_t)token * width,
               (size_t)width * sizeof(f32));
    }
    return record(token_ids->graph, AI_OP_EMBEDDING, token_ids, table, out);
}

AiValue* ai_layer_norm(AiValue* x, AiValue* gamma, AiValue* beta, f32 epsilon) {
    if (!compatible(x, gamma) || !compatible(x, beta) || x->data->ndim != 2 ||
        gamma->data->size != (size_t)x->data->shape[1] ||
        beta->data->size != (size_t)x->data->shape[1] || epsilon <= 0.0f) {
        if (x) set_error(x->graph, "layer norm requires [N,D], gamma [1,D], beta [1,D]");
        return NULL;
    }
    i32 rows = x->data->shape[0], width = x->data->shape[1];
    Tensor* out = tensor_create(2, x->data->shape);
    if (!out) return NULL;
    for (i32 row = 0; row < rows; ++row) {
        const f32* source = x->data->data + (size_t)row * width;
        f32 mean = 0.0f, variance = 0.0f;
        for (i32 j = 0; j < width; ++j) mean += source[j];
        mean /= (f32)width;
        for (i32 j = 0; j < width; ++j) { f32 d = source[j] - mean; variance += d * d; }
        f32 inverse = 1.0f / sqrtf(variance / (f32)width + epsilon);
        for (i32 j = 0; j < width; ++j)
            out->data[(size_t)row * width + j] =
                (source[j] - mean) * inverse * gamma->data->data[j] + beta->data->data[j];
    }
    return record_three(x->graph, AI_OP_LAYER_NORM, x, gamma, beta, out, NULL, 0, epsilon);
}

AiValue* ai_causal_attention(AiValue* query, AiValue* key, AiValue* value,
                             i32 num_heads, i32 sequence_length) {
    if (!compatible(query, key) || !compatible(query, value) ||
        query->data->ndim != 2 || key->data->ndim != 2 || value->data->ndim != 2 ||
        query->data->shape[0] != key->data->shape[0] ||
        query->data->shape[0] != value->data->shape[0] ||
        query->data->shape[1] != key->data->shape[1] ||
        query->data->shape[1] != value->data->shape[1] || num_heads <= 0 ||
        query->data->shape[1] % num_heads != 0 || sequence_length <= 0 ||
        query->data->shape[0] % sequence_length != 0) {
        if (query) set_error(query->graph, "attention requires matching [sequence,hidden] tensors and valid heads");
        return NULL;
    }
    i32 rows = query->data->shape[0], sequence = sequence_length;
    i32 batches = rows / sequence, hidden = query->data->shape[1];
    i32 head_width = hidden / num_heads;
    size_t probability_count = (size_t)batches * num_heads * sequence * sequence;
    f32* probabilities = (f32*)calloc(probability_count, sizeof(f32));
    Tensor* out = tensor_create(2, query->data->shape);
    if (!probabilities || !out) { free(probabilities); tensor_destroy(out); return NULL; }
    f32 scale = 1.0f / sqrtf((f32)head_width);
    for (i32 batch = 0; batch < batches; ++batch) for (i32 head = 0; head < num_heads; ++head) {
        i32 offset = head * head_width;
        for (i32 i = 0; i < sequence; ++i) {
            i32 query_row = batch * sequence + i;
            f32 maximum = -INFINITY;
            for (i32 j = 0; j <= i; ++j) {
                f32 score = 0.0f;
                for (i32 d = 0; d < head_width; ++d)
                    score += query->data->data[(size_t)query_row * hidden + offset + d] *
                             key->data->data[(size_t)(batch * sequence + j) * hidden + offset + d];
                score *= scale;
                probabilities[(((size_t)batch * num_heads + head) * sequence + i) * sequence + j] = score;
                if (score > maximum) maximum = score;
            }
            f32 denominator = 0.0f;
            for (i32 j = 0; j <= i; ++j) {
                size_t p = (((size_t)batch * num_heads + head) * sequence + i) * sequence + j;
                probabilities[p] = expf(probabilities[p] - maximum); denominator += probabilities[p];
            }
            for (i32 j = 0; j <= i; ++j) {
                size_t p = (((size_t)batch * num_heads + head) * sequence + i) * sequence + j;
                probabilities[p] /= denominator;
                for (i32 d = 0; d < head_width; ++d)
                    out->data[(size_t)query_row * hidden + offset + d] +=
                        probabilities[p] * value->data->data[(size_t)(batch * sequence + j) * hidden + offset + d];
            }
        }
    }
    AiValue* result = record_three(query->graph, AI_OP_CAUSAL_ATTENTION,
                                   query, key, value, out, probabilities,
                                   num_heads, 0.0f);
    if (result && result->creator) result->creator->option2 = sequence_length;
    return result;
}

AiValue* ai_cross_entropy(AiValue* logits, AiValue* target_ids) {
    if (!compatible(logits, target_ids) || logits->data->ndim != 2 ||
        target_ids->data->ndim != 2 || target_ids->data->shape[1] != 1 ||
        target_ids->data->shape[0] != logits->data->shape[0]) {
        if (logits) set_error(logits->graph, "cross entropy requires logits [N,C] and targets [N,1]");
        return NULL;
    }
    i32 rows = logits->data->shape[0], classes = logits->data->shape[1];
    Tensor* out = tensor_create(1, (i32[]){1});
    if (!out) return NULL;
    f32 loss = 0.0f;
    for (i32 row = 0; row < rows; ++row) {
        i32 target = (i32)target_ids->data->data[row];
        if (target < 0 || target >= classes) { tensor_destroy(out); set_error(logits->graph, "target id is outside logits"); return NULL; }
        const f32* values = logits->data->data + (size_t)row * classes;
        f32 maximum = values[0], sum = 0.0f;
        for (i32 c = 1; c < classes; ++c) if (values[c] > maximum) maximum = values[c];
        for (i32 c = 0; c < classes; ++c) sum += expf(values[c] - maximum);
        loss += logf(sum) + maximum - values[target];
    }
    out->data[0] = loss / (f32)rows;
    return record(logits->graph, AI_OP_CROSS_ENTROPY, logits, target_ids, out);
}

typedef struct VisitState { unsigned char* marks; AiNode** order; size_t count, capacity; } VisitState;

static bool visit(AiNode* node, VisitState* state) {
    if (!node) return true;
    if (state->marks[node->id] == 2) return true;
    if (state->marks[node->id] == 1) return false;
    state->marks[node->id] = 1;
    for (size_t i = 0; i < node->input_count; i++)
        if (node->inputs[i] && !visit(node->inputs[i]->creator, state)) return false;
    state->marks[node->id] = 2;
    if (state->count == state->capacity) {
        size_t next = state->capacity ? state->capacity * 2 : 16;
        AiNode** resized = (AiNode**)realloc(state->order, next * sizeof(AiNode*));
        if (!resized) return false;
        state->order = resized; state->capacity = next;
    }
    state->order[state->count++] = node;
    return true;
}

bool ai_build_backward_graph(AiValue* loss, AiBackwardGraph* backward) {
    if (!backward) return false;
    memset(backward, 0, sizeof(*backward));
    if (!loss || !loss->graph) return false;
    VisitState state = {0};
    state.marks = (unsigned char*)calloc(loss->graph->node_count, 1);
    if (loss->graph->node_count && !state.marks) return false;
    bool ok = visit(loss->creator, &state);
    free(state.marks);
    if (!ok) { free(state.order); set_error(loss->graph, "cycle or allocation failure in backward graph"); return false; }
    backward->topological = state.order; backward->count = state.count;
    return true;
}

void ai_backward_graph_destroy(AiBackwardGraph* backward) {
    if (backward) { free(backward->topological); memset(backward, 0, sizeof(*backward)); }
}

static Tensor* ensure_grad(AiValue* value) {
    if (!value->grad) { value->grad = tensor_create(value->data->ndim, value->data->shape); tensor_zeros(value->grad); }
    return value->grad;
}

static void accumulate(AiValue* value, size_t index, f32 amount) {
    if (!value || !value->requires_grad) return;
    Tensor* grad = ensure_grad(value);
    grad->data[value->data->size == 1 ? 0 : index] += amount;
}

static void backward_node(AiNode* node) {
    AiValue* out = node->output;
    if (!out->grad) return;
    AiValue* a = node->inputs[0]; AiValue* b = node->inputs[1];
    switch (node->op) {
    case AI_OP_ADD: case AI_OP_SUB: case AI_OP_MUL:
        for (size_t i = 0; i < out->data->size; i++) {
            f32 upstream = out->grad->data[i];
            f32 av = a->data->data[a->data->size == 1 ? 0 : i];
            f32 bv = b->data->data[b->data->size == 1 ? 0 : i];
            accumulate(a, i, node->op == AI_OP_MUL ? upstream * bv : upstream);
            accumulate(b, i, node->op == AI_OP_ADD ? upstream :
                       node->op == AI_OP_SUB ? -upstream : upstream * av);
        }
        break;
    case AI_OP_MATMUL: {
        i32 m = a->data->shape[0], k = a->data->shape[1], n = b->data->shape[1];
        if (a->requires_grad) ensure_grad(a);
        if (b->requires_grad) ensure_grad(b);
        for (i32 i = 0; i < m; i++) for (i32 j = 0; j < n; j++) {
            f32 g = out->grad->data[i * n + j];
            for (i32 p = 0; p < k; p++) {
                if (a->requires_grad) a->grad->data[i * k + p] += g * b->data->data[p * n + j];
                if (b->requires_grad) b->grad->data[p * n + j] += a->data->data[i * k + p] * g;
            }
        }
        break;
    }
    case AI_OP_RELU: case AI_OP_SIGMOID:
        for (size_t i = 0; i < out->data->size; i++) {
            f32 local = node->op == AI_OP_RELU ? (a->data->data[i] > 0 ? 1.0f : 0.0f)
                                                : out->data->data[i] * (1.0f - out->data->data[i]);
            accumulate(a, i, out->grad->data[i] * local);
        }
        break;
    case AI_OP_SUM: case AI_OP_MEAN: {
        f32 scale = node->op == AI_OP_MEAN ? 1.0f / (f32)a->data->size : 1.0f;
        for (size_t i = 0; i < a->data->size; i++) accumulate(a, i, out->grad->data[0] * scale);
        break;
    }
    case AI_OP_EMBEDDING: {
        AiValue* table = b;
        i32 rows = a->data->shape[0], width = table->data->shape[1];
        if (table->requires_grad) ensure_grad(table);
        for (i32 row = 0; row < rows; ++row) {
            i32 token = (i32)a->data->data[row];
            for (i32 d = 0; d < width; ++d)
                if (table->requires_grad)
                    table->grad->data[(size_t)token * width + d] +=
                        out->grad->data[(size_t)row * width + d];
        }
        break;
    }
    case AI_OP_LAYER_NORM: {
        AiValue* gamma = b; AiValue* beta = node->inputs[2];
        i32 rows = a->data->shape[0], width = a->data->shape[1];
        if (a->requires_grad) ensure_grad(a);
        if (gamma->requires_grad) ensure_grad(gamma);
        if (beta->requires_grad) ensure_grad(beta);
        for (i32 row = 0; row < rows; ++row) {
            const f32* source = a->data->data + (size_t)row * width;
            const f32* upstream = out->grad->data + (size_t)row * width;
            f32 mean = 0.0f, variance = 0.0f;
            for (i32 d = 0; d < width; ++d) mean += source[d];
            mean /= (f32)width;
            for (i32 d = 0; d < width; ++d) { f32 centered = source[d] - mean; variance += centered * centered; }
            f32 inverse = 1.0f / sqrtf(variance / (f32)width + node->epsilon);
            f32 sum_dxhat = 0.0f, sum_dxhat_xhat = 0.0f;
            for (i32 d = 0; d < width; ++d) {
                f32 xhat = (source[d] - mean) * inverse;
                f32 dxhat = upstream[d] * gamma->data->data[d];
                sum_dxhat += dxhat; sum_dxhat_xhat += dxhat * xhat;
                if (gamma->requires_grad) gamma->grad->data[d] += upstream[d] * xhat;
                if (beta->requires_grad) beta->grad->data[d] += upstream[d];
            }
            if (a->requires_grad) for (i32 d = 0; d < width; ++d) {
                f32 xhat = (source[d] - mean) * inverse;
                f32 dxhat = upstream[d] * gamma->data->data[d];
                a->grad->data[(size_t)row * width + d] += inverse / (f32)width *
                    ((f32)width * dxhat - sum_dxhat - xhat * sum_dxhat_xhat);
            }
        }
        break;
    }
    case AI_OP_CAUSAL_ATTENTION: {
        AiValue* key = b; AiValue* value = node->inputs[2];
        i32 heads = node->option, sequence = node->option2;
        i32 batches = a->data->shape[0] / sequence;
        i32 hidden = a->data->shape[1], head_width = hidden / heads;
        f32 scale = 1.0f / sqrtf((f32)head_width);
        f32* probabilities = (f32*)node->auxiliary;
        if (a->requires_grad) ensure_grad(a);
        if (key->requires_grad) ensure_grad(key);
        if (value->requires_grad) ensure_grad(value);
        f32* probability_grad = (f32*)malloc((size_t)sequence * sizeof(f32));
        if (!probability_grad) break;
        for (i32 batch = 0; batch < batches; ++batch) for (i32 head = 0; head < heads; ++head) {
            i32 offset = head * head_width;
            for (i32 i = 0; i < sequence; ++i) {
                i32 query_row = batch * sequence + i;
                f32 weighted = 0.0f;
                for (i32 j = 0; j <= i; ++j) {
                    f32 dp = 0.0f;
                    for (i32 d = 0; d < head_width; ++d) {
                        f32 upstream = out->grad->data[(size_t)query_row * hidden + offset + d];
                        dp += upstream * value->data->data[(size_t)(batch * sequence + j) * hidden + offset + d];
                        if (value->requires_grad)
                            value->grad->data[(size_t)(batch * sequence + j) * hidden + offset + d] +=
                                probabilities[(((size_t)batch * heads + head) * sequence + i) * sequence + j] * upstream;
                    }
                    probability_grad[j] = dp;
                    weighted += dp * probabilities[(((size_t)batch * heads + head) * sequence + i) * sequence + j];
                }
                for (i32 j = 0; j <= i; ++j) {
                    f32 ds = probabilities[(((size_t)batch * heads + head) * sequence + i) * sequence + j] *
                             (probability_grad[j] - weighted) * scale;
                    for (i32 d = 0; d < head_width; ++d) {
                        if (a->requires_grad)
                            a->grad->data[(size_t)query_row * hidden + offset + d] +=
                                ds * key->data->data[(size_t)(batch * sequence + j) * hidden + offset + d];
                        if (key->requires_grad)
                            key->grad->data[(size_t)(batch * sequence + j) * hidden + offset + d] +=
                                ds * a->data->data[(size_t)query_row * hidden + offset + d];
                    }
                }
            }
        }
        free(probability_grad);
        break;
    }
    case AI_OP_CROSS_ENTROPY: {
        i32 rows = a->data->shape[0], classes = a->data->shape[1];
        if (a->requires_grad) ensure_grad(a);
        if (!a->requires_grad) break;
        f32 upstream = out->grad->data[0] / (f32)rows;
        for (i32 row = 0; row < rows; ++row) {
            const f32* values = a->data->data + (size_t)row * classes;
            f32 maximum = values[0], sum = 0.0f;
            for (i32 c = 1; c < classes; ++c) if (values[c] > maximum) maximum = values[c];
            for (i32 c = 0; c < classes; ++c) sum += expf(values[c] - maximum);
            i32 target = (i32)b->data->data[row];
            for (i32 c = 0; c < classes; ++c) {
                f32 gradient = expf(values[c] - maximum) / sum;
                if (c == target) gradient -= 1.0f;
                a->grad->data[(size_t)row * classes + c] += upstream * gradient;
            }
        }
        break;
    }
    }
}

bool ai_backward_with_gradient(AiValue* output, const Tensor* gradient) {
    if (!output || !output->requires_grad || !gradient || gradient->size != output->data->size) return false;
    AiBackwardGraph backward;
    if (!ai_build_backward_graph(output, &backward)) return false;
    /* Non-leaf gradients are temporary propagation buffers. Clear them for
     * each backward traversal while preserving PyTorch-style accumulation at
     * leaves/parameters across repeated backward calls. */
    for (size_t i = 0; i < backward.count; i++) {
        AiValue* intermediate = backward.topological[i]->output;
        if (intermediate->grad) tensor_zeros(intermediate->grad);
    }
    Tensor* seed = ensure_grad(output);
    for (size_t i = 0; i < seed->size; i++) seed->data[i] += gradient->data[i];
    for (size_t i = backward.count; i > 0; i--) backward_node(backward.topological[i - 1]);
    ai_backward_graph_destroy(&backward);
    return true;
}

bool ai_backward(AiValue* loss) {
    if (!loss || loss->data->size != 1) { if (loss) set_error(loss->graph, "backward() requires a scalar loss"); return false; }
    Tensor* one = tensor_create(1, (i32[]){1});
    if (!one) return false;
    one->data[0] = 1.0f;
    bool ok = ai_backward_with_gradient(loss, one);
    tensor_destroy(one);
    return ok;
}

void ai_graph_zero_grad(AiGraph* graph) {
    if (!graph) return;
    for (size_t i = 0; i < graph->value_count; i++)
        if (graph->values[i]->grad) tensor_zeros(graph->values[i]->grad);
}

void ai_graph_sgd_step(AiValue** parameters, size_t count, f32 learning_rate) {
    for (size_t p = 0; p < count; p++) {
        AiValue* value = parameters[p];
        if (!value || !value->grad) continue;
        for (size_t i = 0; i < value->data->size; i++)
            value->data->data[i] -= learning_rate * value->grad->data[i];
    }
}
