#include "cpu_collective.h"

#include <math.h>

static bool same_shape(const Tensor* a, const Tensor* b) {
    if (!a || !b || !a->data || !b->data || a->ndim != b->ndim || a->size != b->size ||
        !tensor_is_contiguous(a) || !tensor_is_contiguous(b)) return false;
    for (i32 d = 0; d < a->ndim; d++) if (a->shape[d] != b->shape[d]) return false;
    return true;
}

AiCollectiveStatus ai_cpu_process_group_init(AiCpuProcessGroup* group, int world_size) {
    if (!group || world_size <= 0) return AI_COLLECTIVE_INVALID_ARGUMENT;
    group->world_size = world_size;
    group->completed_collectives = 0;
    return AI_COLLECTIVE_OK;
}

AiCollectiveStatus ai_cpu_allreduce(AiCpuProcessGroup* group,
                                    const Tensor* const* worker_values,
                                    Tensor* output, AiReduceOp operation) {
    if (!group || group->world_size <= 0 || !worker_values || !output ||
        (operation != AI_REDUCE_SUM && operation != AI_REDUCE_MEAN))
        return AI_COLLECTIVE_INVALID_ARGUMENT;
    for (int worker = 0; worker < group->world_size; worker++)
        if (!same_shape(worker_values[worker], output)) return AI_COLLECTIVE_SHAPE_MISMATCH;
    for (int worker = 0; worker < group->world_size; worker++)
        for (size_t i = 0; i < output->size; i++)
            if (!isfinite(worker_values[worker]->data[i])) return AI_COLLECTIVE_NONFINITE;

    /* Fixed worker order plus compensated summation gives deterministic CPU results. */
    for (size_t i = 0; i < output->size; i++) {
        double sum = 0.0, correction = 0.0;
        for (int worker = 0; worker < group->world_size; worker++) {
            double value = worker_values[worker]->data[i];
            double adjusted = value - correction;
            double next = sum + adjusted;
            correction = (next - sum) - adjusted;
            sum = next;
        }
        if (operation == AI_REDUCE_MEAN) sum /= group->world_size;
        output->data[i] = (float)sum;
    }
    group->completed_collectives++;
    return AI_COLLECTIVE_OK;
}

AiCollectiveStatus ai_cpu_sync_gradients(AiCpuProcessGroup* group,
                                         Tensor*** worker_gradients,
                                         int parameter_count, AiReduceOp operation) {
    if (!group || group->world_size <= 0 || !worker_gradients || parameter_count <= 0 ||
        (operation != AI_REDUCE_SUM && operation != AI_REDUCE_MEAN))
        return AI_COLLECTIVE_INVALID_ARGUMENT;
    /* Preflight every shard before synchronizing any parameter. */
    for (int parameter = 0; parameter < parameter_count; parameter++) {
        for (int worker = 0; worker < group->world_size; worker++) {
            if (!worker_gradients[worker]) return AI_COLLECTIVE_INVALID_ARGUMENT;
            if (!same_shape(worker_gradients[worker][parameter], worker_gradients[0][parameter]))
                return AI_COLLECTIVE_SHAPE_MISMATCH;
            for (size_t i = 0; i < worker_gradients[worker][parameter]->size; i++)
                if (!isfinite(worker_gradients[worker][parameter]->data[i]))
                    return AI_COLLECTIVE_NONFINITE;
        }
    }
    for (int parameter = 0; parameter < parameter_count; parameter++) {
        Tensor* output = worker_gradients[0][parameter];
        for (size_t i = 0; i < output->size; i++) {
            double sum = 0.0, correction = 0.0;
            for (int worker = 0; worker < group->world_size; worker++) {
                double value = worker_gradients[worker][parameter]->data[i];
                double adjusted = value - correction;
                double next = sum + adjusted;
                correction = (next - sum) - adjusted;
                sum = next;
            }
            if (operation == AI_REDUCE_MEAN) sum /= group->world_size;
            output->data[i] = (float)sum;
        }
        for (int worker = 1; worker < group->world_size; worker++)
            tensor_copy(worker_gradients[0][parameter], worker_gradients[worker][parameter]);
        group->completed_collectives++;
    }
    return AI_COLLECTIVE_OK;
}
