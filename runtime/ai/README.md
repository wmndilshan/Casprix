# Casprix AI runtime

The AI runtime has one Tensor ABI and separates reusable infrastructure from
model domains.

```text
runtime/ai/
|-- ai.h          stable public umbrella header
|-- core/         Tensor storage and public topological-DAG autograd
|-- kernels/      model-neutral CPU forward/backward kernels and AVX2 assembly
|-- engine/       optimized recording tape, tensor IR, graph passes, and JIT
|-- ops/          Tensor-level activations and losses
|-- training/     shared optimizers, schedules, clipping, and early stopping
|-- evaluation/   classification/regression metrics
|-- distributed/  deterministic CPU data-parallel collectives
|-- classical/    Tensor-based regression and clustering algorithms
|-- nn/           layers, data loaders, and neural-network model composition
|-- llm/          transformer, attention, tokenizer, inference, and checkpoints
`-- backend/      experimental low-level CPU execution/quantization backend
```

## Dependency rules

- `core` depends on no model folder.
- `kernels`, `ops`, `training`, and `evaluation` depend on `core`.
- `engine` combines `core` and `kernels` but is not part of the basic Tensor API.
- `nn`, `llm`, and `classical` consume shared modules; they never define Tensor,
  generic optimizers, or generic kernels.

New C code should normally include `runtime/ai/ai.h`. Specialized model code may
include `nn/nn.h` or individual `llm` headers. There are intentionally no
`llm/tensor.h`, `llm/ops.h`, or `llm/autograd.h` compatibility aliases.

## Tensor and gradients

`Tensor` owns or views reference-counted `TensorStorage`. `tensor_create` is
zero-initialized; full-overwrite kernels can request `tensor_empty` explicitly.
Views retain storage, so destroying a parent descriptor cannot invalidate a live
slice.

`core/autograd` is the public eager reverse-mode graph. It discovers dependencies,
detects cycles, topologically sorts reachable nodes, and propagates gradients in
reverse order. `engine/tape` is a shared optimized recording/lowering mechanism
for transformer-scale fused operations. It is named and located separately because
it is an execution backend, not another Tensor or model-owned autograd API.

## Training and algorithms

`training/optimizer` contains the only stateful SGD/momentum, Adam/AMSGrad,
RMSProp, and AdaGrad implementation. Transformer legacy entry points delegate to
it. CPU collectives synchronize gradients in deterministic worker order without
CUDA dependencies.

`classical/algorithms` implements linear regression, logistic regression, and
k-means directly over the shared Tensor type.

## Verification

`test_ai_autograd` validates graph topology, branch accumulation, views, and
end-to-end differentiation. `test_ai_ml` covers metrics, every shared optimizer,
two-worker gradient synchronization, classical algorithms, and held-out Iris
classification. `test_ai_nn` links the public neural-network surface and verifies
dense forward/backward propagation, shared loss evaluation, and optimizer updates.
