/**
 * ResNet Building Blocks
 * 
 * Residual blocks for deep convolutional networks
 */

#ifndef NN_RESNET_H
#define NN_RESNET_H

#include "../../llm/tensor.h"
#include "../layers/conv2d.h"
#include "../layers/batchnorm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Basic ResNet block (for ResNet-18/34)
 * Conv -> BN -> ReLU -> Conv -> BN -> Add -> ReLU
 */
typedef struct {
    Conv2DLayer* conv1;
    BatchNormLayer* bn1;
    Conv2DLayer* conv2;
    BatchNormLayer* bn2;
    Conv2DLayer* downsample;  /* For dimension matching (optional) */
    BatchNormLayer* bn_downsample;
    
    int stride;
} ResNetBasicBlock;

/**
 * Bottleneck ResNet block (for ResNet-50/101/152)
 * Conv1x1 -> BN -> ReLU -> Conv3x3 -> BN -> ReLU -> Conv1x1 -> BN -> Add -> ReLU
 */
typedef struct {
    Conv2DLayer* conv1;  /* 1x1, reduce */
    BatchNormLayer* bn1;
    Conv2DLayer* conv2;  /* 3x3 */
    BatchNormLayer* bn2;
    Conv2DLayer* conv3;  /* 1x1, expand */
    BatchNormLayer* bn3;
    Conv2DLayer* downsample;
    BatchNormLayer* bn_downsample;
    
    int stride;
} ResNetBottleneck;

/**
 * Create basic ResNet block
 * @param in_channels Input channels
 * @param out_channels Output channels
 * @param stride Stride for first conv
 */
ResNetBasicBlock* resnet_basic_create(int in_channels, int out_channels, int stride);

/**
 * Create bottleneck ResNet block
 * @param in_channels Input channels
 * @param bottleneck_channels Channels in middle layer
 * @param out_channels Output channels (usually 4x bottleneck)
 * @param stride Stride for middle conv
 */
ResNetBottleneck* resnet_bottleneck_create(int in_channels, int bottleneck_channels,
                                           int out_channels, int stride);

/**
 * Forward pass for basic block
 * @param input Input tensor [batch, in_channels, H, W]
 * @param output Output tensor [batch, out_channels, H', W']
 */
void resnet_basic_forward(ResNetBasicBlock* block, const Tensor* input, Tensor* output);

/**
 * Forward pass for bottleneck block
 */
void resnet_bottleneck_forward(ResNetBottleneck* block, const Tensor* input, Tensor* output);

/**
 * Free blocks
 */
void resnet_basic_free(ResNetBasicBlock* block);
void resnet_bottleneck_free(ResNetBottleneck* block);

#ifdef __cplusplus
}
#endif

#endif /* NN_RESNET_H */
