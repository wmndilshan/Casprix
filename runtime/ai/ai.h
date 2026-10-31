#ifndef CPX_AI_H
#define CPX_AI_H

/* Stable public AI surface. Model-specific headers are opt-in. */
#include "core/tensor.h"
#include "core/autograd.h"
#include "training/training.h"
#include "training/optimizer.h"
#include "training/scheduler.h"
#include "ops/activations.h"
#include "ops/losses.h"
#include "distributed/cpu_collective.h"
#include "classical/algorithms.h"
#include "evaluation/metrics.h"

#endif
