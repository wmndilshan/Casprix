# Casprix ML / neural-network runtime (`runtime/ai/nn`)

CPU-oriented neural network building blocks on top of `runtime/ai/llm` (tensors, ops, autograd). Optimized paths use AVX2 where enabled at build time.

## Features

- **Layers**: dense, conv2d, pooling, batch norm, dropout, RNN/LSTM/GRU stubs and related pieces (see headers under `layers/`)
- **Activations**: ReLU, GELU, sigmoid, tanh, etc. (`activations/`)
- **Optimizers**: SGD, Adam, AdamW, Adagrad, RMSprop, schedulers (`optimizers/`)
- **Loss & metrics**: cross-entropy, MSE, Huber, accuracy-style metrics (`loss/`, `metrics/`)
- **Models**: sequential container, checkpoints, ResNet/attention helpers (`models/`)
- **Data**: dataset / dataloader scaffolding (`data/`)

## Layout

```
runtime/ai/nn/
├── nn.h                    # Umbrella include
├── layers/                 # dense, conv2d, pool, batchnorm, dropout, rnn, lstm, gru, …
├── activations/
├── loss/
├── optimizers/
├── models/
├── data/
└── metrics/
```

## Relationship to `runtime/ai/llm`

- **`runtime/ai/llm/tensor.h`** — core `Tensor` type and allocators
- **`runtime/ai/llm/ops.h`** — SIMD-heavy primitives
- **`runtime/ai/llm/autograd.h`** — autograd tape (where linked in)

## Building

These sources are compiled into **`libcasprix_runtime`** when `BUILD_RUNTIME=ON` in the root CMake project. Do not rely on hand-rolled `gcc … **/*.c` one-liners; use the normal CMake build so flags and include paths match the rest of the tree.

## Examples

There is no checked-in `examples/nn_xor_example.c` in this repository; use the CMake-built runtime tests under `tests/runtime/` or the `examples/llm_training/` sketch for higher-level experiments.

## License

MIT — part of the Casprix project.
