# Neural-network module

`runtime/ai/nn` contains model composition only:

- `layers/`: dense, convolution, pooling, normalization, dropout, and recurrent layers
- `models/`: sequential, attention, ResNet, and checkpoint helpers
- `data/`: datasets and data loaders
- `nn.h`: umbrella header for NN consumers

Shared concerns do not live here:

- Tensor and eager autograd: `../core/`
- CPU math kernels: `../kernels/`
- activations and losses: `../ops/`
- optimizers and schedules: `../training/`
- metrics: `../evaluation/`

This boundary prevents NN and transformer code from growing incompatible Tensor,
optimizer, loss, or kernel implementations.
