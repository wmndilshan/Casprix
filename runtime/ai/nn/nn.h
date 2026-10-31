/**
 * Neural Network Library for Casprix
 * 
 * Complete CPU-efficient ML/DL library with SIMD optimizations
 * All models use the shared Tensor ABI and dependency-derived autograd graph.
 */

#ifndef NN_H
#define NN_H

#include "../core/tensor.h"
#include "../kernels/cpu_ops.h"
#include "../core/autograd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * LAYERS
 * ======================================================================== */

#include "layers/dense.h"
#include "layers/conv2d.h"
#include "layers/pool.h"
#include "layers/rnn.h"
#include "layers/lstm.h"
#include "layers/gru.h"
#include "layers/dropout.h"
#include "layers/batchnorm.h"

/* ========================================================================
 * ACTIVATIONS
 * ======================================================================== */

#include "../ops/activations.h"

/* ========================================================================
 * OPTIMIZERS
 * ======================================================================== */

#include "../training/optimizer.h"
#include "../training/training.h"
#include "../training/scheduler.h"

/* ========================================================================
 * LOSS FUNCTIONS
 * ======================================================================== */

#include "../ops/losses.h"

/* ========================================================================
 * DATA LOADING
 * ======================================================================== */

#include "data/dataset.h"
#include "data/dataloader.h"

/* ========================================================================
 * MODELS
 * ======================================================================== */

#include "models/sequential.h"
#include "models/resnet.h"

/* ========================================================================
 * METRICS
 * ======================================================================== */

#include "../evaluation/metrics.h"

/* ========================================================================
 * VERSION INFO
 * ======================================================================== */

#define NN_VERSION_MAJOR 1
#define NN_VERSION_MINOR 0
#define NN_VERSION_PATCH 0

const char* nn_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_H */
