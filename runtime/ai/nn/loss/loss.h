/**
 * Loss Functions
 */

#ifndef NN_LOSS_H
#define NN_LOSS_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Loss Functions
 * ======================================================================== */

/**
 * Mean Squared Error: loss = mean((predictions - targets)^2)
 * @param predictions Predicted values [batch, ...]
 * @param targets Target values [batch, ...]
 * @return Average loss
 */
float mse_loss(const Tensor* predictions, const Tensor* targets);

/**
 * MSE backward: grad = 2 * (predictions - targets) / n
 */
void mse_backward(const Tensor* predictions, const Tensor* targets, Tensor* grad);

/**
 * Binary Cross-Entropy: loss = -mean(y*log(p) + (1-y)*log(1-p))
 * @param predictions Predicted probabilities [batch, ...]
 * @param targets Binary targets (0 or 1) [batch, ...]
 */
float binary_cross_entropy_loss(const Tensor* predictions, const Tensor* targets);

/**
 * Binary cross-entropy backward
 */
void binary_cross_entropy_backward(const Tensor* predictions, const Tensor* targets, 
                                  Tensor* grad);

/**
 * Categorical Cross-Entropy with softmax
 * @param logits Raw logits [batch, num_classes]
 * @param targets Class indices [batch] (as float tensor)
 * @return Average loss
 */
float categorical_cross_entropy_loss(const Tensor* logits, const Tensor* targets);

/**
 * Categorical cross-entropy backward (fused with softmax)
 * @param logits Raw logits [batch, num_classes]
 * @param targets Class indices [batch]
 * @param grad Gradient output [batch, num_classes]
 */
void categorical_cross_entropy_backward(const Tensor* logits, const Tensor* targets, 
                                       Tensor* grad);

/**
 * L1 Loss (Mean Absolute Error): loss = mean(|predictions - targets|)
 */
float l1_loss(const Tensor* predictions, const Tensor* targets);

/**
 * L1 backward
 */
void l1_backward(const Tensor* predictions, const Tensor* targets, Tensor* grad);

/**
 * Huber Loss (smooth L1): combines MSE and L1
 */
float huber_loss(const Tensor* predictions, const Tensor* targets, float delta);

/**
 * Huber backward
 */
void huber_backward(const Tensor* predictions, const Tensor* targets, 
                   Tensor* grad, float delta);

#ifdef __cplusplus
}
#endif

#endif /* NN_LOSS_H */
