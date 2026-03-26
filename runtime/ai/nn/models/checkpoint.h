/**
 * Model Checkpointing
 * 
 * Save and load model weights
 */

#ifndef NN_CHECKPOINT_H
#define NN_CHECKPOINT_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Save model parameters to file
 * @param filepath Path to save checkpoint
 * @param params Array of parameter tensors
 * @param num_params Number of parameters
 * @param metadata Optional metadata string
 * @return true on success
 */
bool checkpoint_save(const char* filepath, Tensor** params, int num_params,
                    const char* metadata);

/**
 * Load model parameters from file
 * @param filepath Path to load checkpoint
 * @param params Array of parameter tensors (pre-allocated with correct shapes)
 * @param num_params Number of parameters
 * @return true on success
 */
bool checkpoint_load(const char* filepath, Tensor** params, int num_params);

/**
 * Get metadata from checkpoint
 * @param filepath Path to checkpoint file
 * @param metadata_out Output buffer for metadata (caller must free)
 * @return true on success
 */
bool checkpoint_get_metadata(const char* filepath, char** metadata_out);

#ifdef __cplusplus
}
#endif

#endif /* NN_CHECKPOINT_H */
