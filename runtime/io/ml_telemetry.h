#ifndef CASPRIX_ML_TELEMETRY_H
#define CASPRIX_ML_TELEMETRY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "direct_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CPX_TLM_TEXT_MAX
#define CPX_TLM_TEXT_MAX 96
#endif

typedef struct {
    uint64_t step;
    uint32_t worker_id;
    float    loss;
    float    grad_norm;
    float    lr;
    uint16_t text_len;
    char     text[CPX_TLM_TEXT_MAX];
} CpxTelemetryEvent;

typedef struct {
    CpxTelemetryEvent* slots;
    uint32_t           cap;
    uint32_t           mask;
    _Atomic uint32_t   write_idx;
    _Atomic uint32_t   read_idx;
    _Atomic uint64_t   dropped;
} CpxTelemetryShard;

typedef struct {
    CpxTelemetryShard* shards;
    uint32_t           shard_count;
    _Atomic bool       running;
    int                out_fd;
    uintptr_t          drain_thread;
} CpxTelemetryQueue;

/* Each shard is SPSC: one producer thread + one drain thread (wait-free push). */
int  cpx_tlm_init(CpxTelemetryQueue* q, uint32_t shard_count, uint32_t shard_capacity_pow2, int out_fd);
void cpx_tlm_shutdown(CpxTelemetryQueue* q);
bool cpx_tlm_try_push(CpxTelemetryQueue* q, uint32_t shard_id, const CpxTelemetryEvent* ev);
uint64_t cpx_tlm_dropped_total(const CpxTelemetryQueue* q);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_ML_TELEMETRY_H */
