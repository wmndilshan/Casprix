/**
 * Neural Network Library for Casperix
 * 
 * Complete CPU-efficient ML/DL library with SIMD optimizations
 * Building on runtime/llm tensor and autograd infrastructure
 */

#ifndef NN_H
#define NN_H

#include "../llm/tensor.h"
#include "../llm/ops.h"
#include "../llm/autograd.h"

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

#include "activations/activations.h"

/* ========================================================================
 * OPTIMIZERS
 * ======================================================================== */

#include "optimizers/sgd.h"
#include "optimizers/adam.h"
#include "optimizers/rmsprop.h"
#include "optimizers/scheduler.h"

/* ========================================================================
 * LOSS FUNCTIONS
 * ======================================================================== */

#include "loss/loss.h"

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

#include "metrics/metrics.h"

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
