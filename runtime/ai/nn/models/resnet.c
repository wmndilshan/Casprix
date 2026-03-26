/**
 * ResNet Implementation
 */

#include "resnet.h"
#include "../activations/activations.h"
#include "../../llm/ops.h"
#include <stdlib.h>

ResNetBasicBlock* resnet_basic_create(int in_channels, int out_channels, int stride) {
    ResNetBasicBlock* block = (ResNetBasicBlock*)calloc(1, sizeof(ResNetBasicBlock));
    
    block->stride = stride;
    
    /* First conv */
    block->conv1 = conv2d_create(in_channels, out_channels, 3, 3, stride, stride, 1, 1, false);
    block->bn1 = batchnorm_create(out_channels, 0.1f, 1e-5f);
    
    /* Second conv */
    block->conv2 = conv2d_create(out_channels, out_channels, 3, 3, 1, 1, 1, 1, false);
    block->bn2 = batchnorm_create(out_channels, 0.1f, 1e-5f);
    
    /* Downsample if needed (stride > 1 or channels mismatch) */
    if (stride != 1 || in_channels != out_channels) {
        block->downsample = conv2d_create(in_channels, out_channels, 1, 1, stride, stride, 0, 0, false);
        block->bn_downsample = batchnorm_create(out_channels, 0.1f, 1e-5f);
    }
    
    return block;
}

ResNetBottleneck* resnet_bottleneck_create(int in_channels, int bottleneck_channels,
                                           int out_channels, int stride) {
    ResNetBottleneck* block = (ResNetBottleneck*)calloc(1, sizeof(ResNetBottleneck));
    
    block->stride = stride;
    
    /* 1x1 reduce */
    block->conv1 = conv2d_create(in_channels, bottleneck_channels, 1, 1, 1, 1, 0, 0, false);
    block->bn1 = batchnorm_create(bottleneck_channels, 0.1f, 1e-5f);
    
    /* 3x3 */
    block->conv2 = conv2d_create(bottleneck_channels, bottleneck_channels, 3, 3, stride, stride, 1, 1, false);
    block->bn2 = batchnorm_create(bottleneck_channels, 0.1f, 1e-5f);
    
    /* 1x1 expand */
    block->conv3 = conv2d_create(bottleneck_channels, out_channels, 1, 1, 1, 1, 0, 0, false);
    block->bn3 = batchnorm_create(out_channels, 0.1f, 1e-5f);
    
    /* Downsample if needed */
    if (stride != 1 || in_channels != out_channels) {
        block->downsample = conv2d_create(in_channels, out_channels, 1, 1, stride, stride, 0, 0, false);
        block->bn_downsample = batchnorm_create(out_channels, 0.1f, 1e-5f);
    }
    
    return block;
}

void resnet_basic_forward(ResNetBasicBlock* block, const Tensor* input, Tensor* output) {
    /* Determine output size */
    int batch = input->shape[0];
    int channels_out = block->conv1->out_channels;
    int H_out, W_out;
    conv2d_output_size(input->shape[2], input->shape[3],
                      block->conv1->kernel_h, block->conv1->kernel_w,
                      block->conv1->stride_h, block->conv1->stride_w,
                      block->conv1->pad_h, block->conv1->pad_w,
                      &H_out, &W_out);
    
    int temp_shape[] = {batch, channels_out, H_out, W_out};
    Tensor* temp1 = tensor_create(4, temp_shape);
    Tensor* temp2 = tensor_create(4, temp_shape);
    Tensor* identity = tensor_create(4, temp_shape);
    
    /* Conv1 -> BN1 -> ReLU */
    conv2d_forward(block->conv1, input, temp1);
    batchnorm_forward(block->bn1, temp1, temp2);
    relu_f32(temp2->data, temp1->data, temp2->size);
    
    /* Conv2 -> BN2 */
    conv2d_forward(block->conv2, temp1, temp2);
    batchnorm_forward(block->bn2, temp2, temp1);
    
    /* Identity/Downsample */
    if (block->downsample) {
        conv2d_forward(block->downsample, input, temp2);
        batchnorm_forward(block->bn_downsample, temp2, identity);
    } else {
        tensor_copy(input, identity);
    }
    
    /* Add residual */
    vec_add_f32(temp1->data, identity->data, output->data, temp1->size);
    
    /* ReLU */
    relu_f32(output->data, output->data, output->size);
    
    /* Cleanup */
    tensor_destroy(temp1);
    tensor_destroy(temp2);
    tensor_destroy(identity);
}

void resnet_bottleneck_forward(ResNetBottleneck* block, const Tensor* input, Tensor* output) {
    /* Similar to basic but with 3 convs */
    int batch = input->shape[0];
    int channels_out = block->conv3->out_channels;
    int H_out, W_out;
    
    /* Calculate final output size based on stride in conv2 */
    conv2d_output_size(input->shape[2], input->shape[3],
                      block->conv2->kernel_h, block->conv2->kernel_w,
                      block->conv2->stride_h, block->conv2->stride_w,
                      block->conv2->pad_h, block->conv2->pad_w,
                      &H_out, &W_out);
    
    int final_shape[] = {batch, channels_out, H_out, W_out};
    Tensor* temp1 = tensor_create(4, final_shape);
    Tensor* temp2 = tensor_create(4, final_shape);
    Tensor* identity = tensor_create(4, final_shape);
    
    /* Conv1 (1x1) -> BN1 -> ReLU */
    int inter_shape1[] = {batch, block->conv1->out_channels, input->shape[2], input->shape[3]};
    Tensor* inter1 = tensor_create(4, inter_shape1);
    conv2d_forward(block->conv1, input, inter1);
    batchnorm_forward(block->bn1, inter1, temp1);
    relu_f32(temp1->data, inter1->data, inter1->size);
    
    /* Conv2 (3x3) -> BN2 -> ReLU */
    int inter_shape2[] = {batch, block->conv2->out_channels, H_out, W_out};
    Tensor* inter2 = tensor_create(4, inter_shape2);
    conv2d_forward(block->conv2, inter1, inter2);
    batchnorm_forward(block->bn2, inter2, temp1);
    relu_f32(temp1->data, temp1->data, temp1->size);
    
    /* Conv3 (1x1) -> BN3 */
    conv2d_forward(block->conv3, temp1, temp2);
    batchnorm_forward(block->bn3, temp2, temp1);
    
    /* Identity/Downsample */
    if (block->downsample) {
        conv2d_forward(block->downsample, input, temp2);
        batchnorm_forward(block->bn_downsample, temp2, identity);
    } else {
        tensor_copy(input, identity);
    }
    
    /* Add + ReLU */
    vec_add_f32(temp1->data, identity->data, output->data, temp1->size);
    relu_f32(output->data, output->data, output->size);
    
    /* Cleanup */
    tensor_destroy(temp1);
    tensor_destroy(temp2);
    tensor_destroy(identity);
    tensor_destroy(inter1);
    tensor_destroy(inter2);
}

void resnet_basic_free(ResNetBasicBlock* block) {
    if (!block) return;
    
    conv2d_free(block->conv1);
    batchnorm_free(block->bn1);
    conv2d_free(block->conv2);
    batchnorm_free(block->bn2);
    
    if (block->downsample) {
        conv2d_free(block->downsample);
        batchnorm_free(block->bn_downsample);
    }
    
    free(block);
}

void resnet_bottleneck_free(ResNetBottleneck* block) {
    if (!block) return;
    
    conv2d_free(block->conv1);
    batchnorm_free(block->bn1);
    conv2d_free(block->conv2);
    batchnorm_free(block->bn2);
    conv2d_free(block->conv3);
    batchnorm_free(block->bn3);
    
    if (block->downsample) {
        conv2d_free(block->downsample);
        batchnorm_free(block->bn_downsample);
    }
    
    free(block);
}
