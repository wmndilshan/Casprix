#ifndef CPX_AI_CPU_COLLECTIVE_H
#define CPX_AI_CPU_COLLECTIVE_H

#include "../core/tensor.h"

typedef enum AiReduceOp { AI_REDUCE_SUM = 0, AI_REDUCE_MEAN = 1 } AiReduceOp;
typedef enum AiCollectiveStatus {
    AI_COLLECTIVE_OK = 0,
    AI_COLLECTIVE_INVALID_ARGUMENT,
    AI_COLLECTIVE_SHAPE_MISMATCH,
    AI_COLLECTIVE_NONFINITE
} AiCollectiveStatus;

typedef struct AiCpuProcessGroup {
    int world_size;
    size_t completed_collectives;
} AiCpuProcessGroup;

AiCollectiveStatus ai_cpu_process_group_init(AiCpuProcessGroup* group, int world_size);
AiCollectiveStatus ai_cpu_allreduce(AiCpuProcessGroup* group,
                                    const Tensor* const* worker_values,
                                    Tensor* output, AiReduceOp operation);
AiCollectiveStatus ai_cpu_sync_gradients(AiCpuProcessGroup* group,
                                         Tensor*** worker_gradients,
                                         int parameter_count, AiReduceOp operation);

#endif
